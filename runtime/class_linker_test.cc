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

#include "class_linker.h"

#include <string>

#include "UniquePtr.h"
#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "dex_file.h"
#include "entrypoints/entrypoint_utils.h"
#include "gc/heap.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/proxy.h"
#include "mirror/reference.h"
#include "mirror/stack_trace_element.h"
#include "handle_scope-inl.h"

namespace art {

class ClassLinkerTest : public CommonRuntimeTest {
 protected:
  void AssertNonExistentClass(const std::string& descriptor)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    EXPECT_TRUE(class_linker_->FindSystemClass(self, descriptor.c_str()) == NULL);
    EXPECT_TRUE(self->IsExceptionPending());
    mirror::Object* exception = self->GetException(NULL);
    self->ClearException();
    mirror::Class* exception_class =
        class_linker_->FindSystemClass(self, "Ljava/lang/NoClassDefFoundError;");
    EXPECT_TRUE(exception->InstanceOf(exception_class));
  }

  void AssertPrimitiveClass(const std::string& descriptor)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    AssertPrimitiveClass(descriptor, class_linker_->FindSystemClass(self, descriptor.c_str()));
  }

  void AssertPrimitiveClass(const std::string& descriptor, mirror::Class* primitive)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ClassHelper primitive_ch(primitive);
    ASSERT_TRUE(primitive != NULL);
    ASSERT_TRUE(primitive->GetClass() != NULL);
    ASSERT_EQ(primitive->GetClass(), primitive->GetClass()->GetClass());
    EXPECT_TRUE(primitive->GetClass()->GetSuperClass() != NULL);
    ASSERT_STREQ(descriptor.c_str(), primitive_ch.GetDescriptor());
    EXPECT_TRUE(primitive->GetSuperClass() == NULL);
    EXPECT_FALSE(primitive->HasSuperClass());
    EXPECT_TRUE(primitive->GetClassLoader() == NULL);
    EXPECT_EQ(mirror::Class::kStatusInitialized, primitive->GetStatus());
    EXPECT_FALSE(primitive->IsErroneous());
    EXPECT_TRUE(primitive->IsLoaded());
    EXPECT_TRUE(primitive->IsResolved());
    EXPECT_TRUE(primitive->IsVerified());
    EXPECT_TRUE(primitive->IsInitialized());
    EXPECT_FALSE(primitive->IsArrayInstance());
    EXPECT_FALSE(primitive->IsArrayClass());
    EXPECT_TRUE(primitive->GetComponentType() == NULL);
    EXPECT_FALSE(primitive->IsInterface());
    EXPECT_TRUE(primitive->IsPublic());
    EXPECT_TRUE(primitive->IsFinal());
    EXPECT_TRUE(primitive->IsPrimitive());
    EXPECT_FALSE(primitive->IsSynthetic());
    EXPECT_EQ(0U, primitive->NumDirectMethods());
    EXPECT_EQ(0U, primitive->NumVirtualMethods());
    EXPECT_EQ(0U, primitive->NumInstanceFields());
    EXPECT_EQ(0U, primitive->NumStaticFields());
    EXPECT_EQ(0U, primitive_ch.NumDirectInterfaces());
    EXPECT_TRUE(primitive->GetVTable() == NULL);
    EXPECT_EQ(0, primitive->GetIfTableCount());
    EXPECT_TRUE(primitive->GetIfTable() == NULL);
    EXPECT_EQ(kAccPublic | kAccFinal | kAccAbstract, primitive->GetAccessFlags());
  }

  void AssertArrayClass(const std::string& array_descriptor,
                        const std::string& component_type,
                        mirror::ClassLoader* class_loader)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    StackHandleScope<2> hs(self);
    Handle<mirror::ClassLoader> loader(hs.NewHandle(class_loader));
    Handle<mirror::Class> array(
        hs.NewHandle(class_linker_->FindClass(self, array_descriptor.c_str(), loader)));
    ClassHelper array_component_ch(array->GetComponentType());
    EXPECT_STREQ(component_type.c_str(), array_component_ch.GetDescriptor());
    EXPECT_EQ(class_loader, array->GetClassLoader());
    EXPECT_EQ(kAccFinal | kAccAbstract, (array->GetAccessFlags() & (kAccFinal | kAccAbstract)));
    AssertArrayClass(array_descriptor, array);
  }

  void AssertArrayClass(const std::string& array_descriptor, const Handle<mirror::Class>& array)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ClassHelper kh(array.Get());
    ASSERT_TRUE(array.Get() != NULL);
    ASSERT_TRUE(array->GetClass() != NULL);
    ASSERT_EQ(array->GetClass(), array->GetClass()->GetClass());
    EXPECT_TRUE(array->GetClass()->GetSuperClass() != NULL);
    ASSERT_STREQ(array_descriptor.c_str(), kh.GetDescriptor());
    EXPECT_TRUE(array->GetSuperClass() != NULL);
    Thread* self = Thread::Current();
    EXPECT_EQ(class_linker_->FindSystemClass(self, "Ljava/lang/Object;"), array->GetSuperClass());
    EXPECT_TRUE(array->HasSuperClass());
    ASSERT_TRUE(array->GetComponentType() != NULL);
    kh.ChangeClass(array->GetComponentType());
    ASSERT_TRUE(kh.GetDescriptor() != NULL);
    EXPECT_EQ(mirror::Class::kStatusInitialized, array->GetStatus());
    EXPECT_FALSE(array->IsErroneous());
    EXPECT_TRUE(array->IsLoaded());
    EXPECT_TRUE(array->IsResolved());
    EXPECT_TRUE(array->IsVerified());
    EXPECT_TRUE(array->IsInitialized());
    EXPECT_FALSE(array->IsArrayInstance());
    EXPECT_TRUE(array->IsArrayClass());
    EXPECT_FALSE(array->IsInterface());
    EXPECT_EQ(array->GetComponentType()->IsPublic(), array->IsPublic());
    EXPECT_TRUE(array->IsFinal());
    EXPECT_FALSE(array->IsPrimitive());
    EXPECT_FALSE(array->IsSynthetic());
    EXPECT_EQ(0U, array->NumDirectMethods());
    EXPECT_EQ(0U, array->NumVirtualMethods());
    EXPECT_EQ(0U, array->NumInstanceFields());
    EXPECT_EQ(0U, array->NumStaticFields());
    kh.ChangeClass(array.Get());
    EXPECT_EQ(2U, kh.NumDirectInterfaces());
    EXPECT_TRUE(array->GetVTable() != NULL);
    EXPECT_EQ(2, array->GetIfTableCount());
    ASSERT_TRUE(array->GetIfTable() != NULL);
    kh.ChangeClass(kh.GetDirectInterface(0));
    EXPECT_STREQ(kh.GetDescriptor(), "Ljava/lang/Cloneable;");
    kh.ChangeClass(array.Get());
    kh.ChangeClass(kh.GetDirectInterface(1));
    EXPECT_STREQ(kh.GetDescriptor(), "Ljava/io/Serializable;");
    EXPECT_EQ(class_linker_->FindArrayClass(self, array->GetComponentType()), array.Get());
  }

  void AssertMethod(mirror::ArtMethod* method) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    MethodHelper mh(method);
    EXPECT_TRUE(method != NULL);
    EXPECT_TRUE(method->GetClass() != NULL);
    EXPECT_TRUE(mh.GetName() != NULL);
    EXPECT_TRUE(mh.GetSignature() != Signature::NoSignature());

    EXPECT_TRUE(method->GetDexCacheStrings() != NULL);
    EXPECT_TRUE(method->GetDexCacheResolvedMethods() != NULL);
    EXPECT_TRUE(method->GetDexCacheResolvedTypes() != NULL);
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetStrings(),
              method->GetDexCacheStrings());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetResolvedMethods(),
              method->GetDexCacheResolvedMethods());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetResolvedTypes(),
              method->GetDexCacheResolvedTypes());
  }

  void AssertField(mirror::Class* klass, mirror::ArtField* field)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    FieldHelper fh(field);
    EXPECT_TRUE(field != NULL);
    EXPECT_TRUE(field->GetClass() != NULL);
    EXPECT_EQ(klass, field->GetDeclaringClass());
    EXPECT_TRUE(fh.GetName() != NULL);
    EXPECT_TRUE(fh.GetType() != NULL);
  }

  void AssertClass(const std::string& descriptor, const Handle<mirror::Class>& klass)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ClassHelper kh(klass.Get());
    EXPECT_STREQ(descriptor.c_str(), kh.GetDescriptor());
    if (descriptor == "Ljava/lang/Object;") {
      EXPECT_FALSE(klass->HasSuperClass());
    } else {
      EXPECT_TRUE(klass->HasSuperClass());
      EXPECT_TRUE(klass->GetSuperClass() != NULL);
    }
    EXPECT_TRUE(klass->GetClass() != NULL);
    EXPECT_EQ(klass->GetClass(), klass->GetClass()->GetClass());
    EXPECT_TRUE(klass->GetDexCache() != NULL);
    EXPECT_TRUE(klass->IsLoaded());
    EXPECT_TRUE(klass->IsResolved());
    EXPECT_FALSE(klass->IsErroneous());
    EXPECT_FALSE(klass->IsArrayClass());
    EXPECT_TRUE(klass->GetComponentType() == NULL);
    EXPECT_TRUE(klass->IsInSamePackage(klass.Get()));
    EXPECT_TRUE(mirror::Class::IsInSamePackage(kh.GetDescriptor(), kh.GetDescriptor()));
    if (klass->IsInterface()) {
      EXPECT_TRUE(klass->IsAbstract());
      if (klass->NumDirectMethods() == 1) {
        MethodHelper mh(klass->GetDirectMethod(0));
        EXPECT_TRUE(mh.IsClassInitializer());
        EXPECT_TRUE(klass->GetDirectMethod(0)->IsDirect());
      } else {
        EXPECT_EQ(0U, klass->NumDirectMethods());
      }
    } else {
      if (!klass->IsSynthetic()) {
        EXPECT_NE(0U, klass->NumDirectMethods());
      }
    }
    EXPECT_EQ(klass->IsInterface(), klass->GetVTable() == NULL);
    mirror::IfTable* iftable = klass->GetIfTable();
    for (int i = 0; i < klass->GetIfTableCount(); i++) {
      mirror::Class* interface = iftable->GetInterface(i);
      ASSERT_TRUE(interface != NULL);
      if (klass->IsInterface()) {
        EXPECT_EQ(0U, iftable->GetMethodArrayCount(i));
      } else {
        EXPECT_EQ(interface->NumVirtualMethods(), iftable->GetMethodArrayCount(i));
      }
    }
    if (klass->IsAbstract()) {
      EXPECT_FALSE(klass->IsFinal());
    } else {
      EXPECT_FALSE(klass->IsAnnotation());
    }
    if (klass->IsFinal()) {
      EXPECT_FALSE(klass->IsAbstract());
      EXPECT_FALSE(klass->IsAnnotation());
    }
    if (klass->IsAnnotation()) {
      EXPECT_FALSE(klass->IsFinal());
      EXPECT_TRUE(klass->IsAbstract());
    }

    EXPECT_FALSE(klass->IsPrimitive());
    EXPECT_TRUE(klass->CanAccess(klass.Get()));

    for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
      mirror::ArtMethod* method = klass->GetDirectMethod(i);
      AssertMethod(method);
      EXPECT_TRUE(method->IsDirect());
      EXPECT_EQ(klass.Get(), method->GetDeclaringClass());
    }

    for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
      mirror::ArtMethod* method = klass->GetVirtualMethod(i);
      AssertMethod(method);
      EXPECT_FALSE(method->IsDirect());
      EXPECT_TRUE(method->GetDeclaringClass()->IsAssignableFrom(klass.Get()));
    }

    for (size_t i = 0; i < klass->NumInstanceFields(); i++) {
      mirror::ArtField* field = klass->GetInstanceField(i);
      AssertField(klass.Get(), field);
      EXPECT_FALSE(field->IsStatic());
    }

    for (size_t i = 0; i < klass->NumStaticFields(); i++) {
      mirror::ArtField* field = klass->GetStaticField(i);
      AssertField(klass.Get(), field);
      EXPECT_TRUE(field->IsStatic());
    }

    // Confirm that all instances fields are packed together at the start
    EXPECT_GE(klass->NumInstanceFields(), klass->NumReferenceInstanceFields());
    FieldHelper fh;
    for (size_t i = 0; i < klass->NumReferenceInstanceFields(); i++) {
      mirror::ArtField* field = klass->GetInstanceField(i);
      fh.ChangeField(field);
      ASSERT_TRUE(!fh.IsPrimitiveType());
      mirror::Class* field_type = fh.GetType();
      ASSERT_TRUE(field_type != NULL);
      ASSERT_TRUE(!field_type->IsPrimitive());
    }
    for (size_t i = klass->NumReferenceInstanceFields(); i < klass->NumInstanceFields(); i++) {
      mirror::ArtField* field = klass->GetInstanceField(i);
      fh.ChangeField(field);
      mirror::Class* field_type = fh.GetType();
      ASSERT_TRUE(field_type != NULL);
      if (!fh.IsPrimitiveType() || !field_type->IsPrimitive()) {
        // While Reference.referent is not primitive, the ClassLinker
        // treats it as such so that the garbage collector won't scan it.
        EXPECT_EQ(PrettyField(field), "java.lang.Object java.lang.ref.Reference.referent");
      }
    }

    size_t total_num_reference_instance_fields = 0;
    mirror::Class* k = klass.Get();
    while (k != NULL) {
      total_num_reference_instance_fields += k->NumReferenceInstanceFields();
      k = k->GetSuperClass();
    }
    EXPECT_EQ(klass->GetReferenceInstanceOffsets() == 0, total_num_reference_instance_fields == 0);
  }

  void AssertDexFileClass(mirror::ClassLoader* class_loader, const std::string& descriptor)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ASSERT_TRUE(descriptor != NULL);
    Thread* self = Thread::Current();
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> klass(
        hs.NewHandle(class_linker_->FindSystemClass(self, descriptor.c_str())));
    ASSERT_TRUE(klass.Get() != nullptr);
    EXPECT_STREQ(descriptor.c_str(), ClassHelper(klass.Get()).GetDescriptor());
    EXPECT_EQ(class_loader, klass->GetClassLoader());
    if (klass->IsPrimitive()) {
      AssertPrimitiveClass(descriptor, klass.Get());
    } else if (klass->IsArrayClass()) {
      AssertArrayClass(descriptor, klass);
    } else {
      AssertClass(descriptor, klass);
    }
  }

  void AssertDexFile(const DexFile* dex, mirror::ClassLoader* class_loader)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ASSERT_TRUE(dex != NULL);

    // Verify all the classes defined in this file
    for (size_t i = 0; i < dex->NumClassDefs(); i++) {
      const DexFile::ClassDef& class_def = dex->GetClassDef(i);
      const char* descriptor = dex->GetClassDescriptor(class_def);
      AssertDexFileClass(class_loader, descriptor);
    }
    // Verify all the types referenced by this file
    for (size_t i = 0; i < dex->NumTypeIds(); i++) {
      const DexFile::TypeId& type_id = dex->GetTypeId(i);
      const char* descriptor = dex->GetTypeDescriptor(type_id);
      AssertDexFileClass(class_loader, descriptor);
    }
    class_linker_->VisitRoots(TestRootVisitor, NULL, kVisitRootFlagAllRoots);
    // Verify the dex cache has resolution methods in all resolved method slots
    mirror::DexCache* dex_cache = class_linker_->FindDexCache(*dex);
    mirror::ObjectArray<mirror::ArtMethod>* resolved_methods = dex_cache->GetResolvedMethods();
    for (size_t i = 0; i < static_cast<size_t>(resolved_methods->GetLength()); i++) {
      EXPECT_TRUE(resolved_methods->Get(i) != NULL) << dex->GetLocation() << " i=" << i;
    }
  }

  static void TestRootVisitor(mirror::Object** root, void*, uint32_t, RootType) {
    EXPECT_TRUE(*root != NULL);
  }
};

struct CheckOffset {
  size_t cpp_offset;
  const char* java_name;
  CheckOffset(size_t c, const char* j) : cpp_offset(c), java_name(j) {}
};

template <typename T>
struct CheckOffsets {
  CheckOffsets(bool is_static, const char* class_descriptor)
      : is_static(is_static), class_descriptor(class_descriptor) {}
  bool is_static;
  std::string class_descriptor;
  std::vector<CheckOffset> offsets;

  bool Check() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    mirror::Class* klass =
        Runtime::Current()->GetClassLinker()->FindSystemClass(self, class_descriptor.c_str());
    CHECK(klass != NULL) << class_descriptor;

    bool error = false;

    if (!klass->IsClassClass() && !is_static) {
      size_t expected_size = is_static ? klass->GetClassSize(): klass->GetObjectSize();
      if (sizeof(T) != expected_size) {
        LOG(ERROR) << "Class size mismatch:"
           << " class=" << class_descriptor
           << " Java=" << expected_size
           << " C++=" << sizeof(T);
        error = true;
      }
    }

    size_t num_fields = is_static ? klass->NumStaticFields() : klass->NumInstanceFields();
    if (offsets.size() != num_fields) {
      LOG(ERROR) << "Field count mismatch:"
         << " class=" << class_descriptor
         << " Java=" << num_fields
         << " C++=" << offsets.size();
      error = true;
    }

    FieldHelper fh;
    for (size_t i = 0; i < offsets.size(); i++) {
      mirror::ArtField* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
      fh.ChangeField(field);
      StringPiece field_name(fh.GetName());
      if (field_name != offsets[i].java_name) {
        error = true;
      }
    }
    if (error) {
      for (size_t i = 0; i < offsets.size(); i++) {
        CheckOffset& offset = offsets[i];
        mirror::ArtField* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
        fh.ChangeField(field);
        StringPiece field_name(fh.GetName());
        if (field_name != offsets[i].java_name) {
          LOG(ERROR) << "JAVA FIELD ORDER MISMATCH NEXT LINE:";
        }
        LOG(ERROR) << "Java field order:"
           << " i=" << i << " class=" << class_descriptor
           << " Java=" << field_name
           << " CheckOffsets=" << offset.java_name;
      }
    }

    for (size_t i = 0; i < offsets.size(); i++) {
      CheckOffset& offset = offsets[i];
      mirror::ArtField* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
      if (field->GetOffset().Uint32Value() != offset.cpp_offset) {
        error = true;
      }
    }
    if (error) {
      for (size_t i = 0; i < offsets.size(); i++) {
        CheckOffset& offset = offsets[i];
        mirror::ArtField* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
        if (field->GetOffset().Uint32Value() != offset.cpp_offset) {
          LOG(ERROR) << "OFFSET MISMATCH NEXT LINE:";
        }
        LOG(ERROR) << "Offset: class=" << class_descriptor << " field=" << offset.java_name
           << " Java=" << field->GetOffset().Uint32Value() << " C++=" << offset.cpp_offset;
      }
    }

    return !error;
  };

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(CheckOffsets);
};

// Note that ClassLinkerTest.ValidateFieldOrderOfJavaCppUnionClasses
// is first since if it is failing, others are unlikely to succeed.

struct ObjectOffsets : public CheckOffsets<mirror::Object> {
  ObjectOffsets() : CheckOffsets<mirror::Object>(false, "Ljava/lang/Object;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Object, klass_),   "shadow$_klass_"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Object, monitor_), "shadow$_monitor_"));
#ifdef USE_BAKER_OR_BROOKS_READ_BARRIER
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Object, x_rb_ptr_), "shadow$_x_rb_ptr_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Object, x_xpadding_), "shadow$_x_xpadding_"));
#endif
  };
};

struct ArtFieldOffsets : public CheckOffsets<mirror::ArtField> {
  ArtFieldOffsets() : CheckOffsets<mirror::ArtField>(false, "Ljava/lang/reflect/ArtField;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtField, declaring_class_), "declaringClass"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtField, access_flags_),    "accessFlags"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtField, field_dex_idx_),   "fieldDexIndex"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtField, offset_),          "offset"));
  };
};

struct ArtMethodOffsets : public CheckOffsets<mirror::ArtMethod> {
  ArtMethodOffsets() : CheckOffsets<mirror::ArtMethod>(false, "Ljava/lang/reflect/ArtMethod;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, declaring_class_),                      "declaringClass"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, dex_cache_resolved_methods_),           "dexCacheResolvedMethods"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, dex_cache_resolved_types_),             "dexCacheResolvedTypes"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, dex_cache_strings_),                    "dexCacheStrings"));

    // alphabetical 64-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, entry_point_from_interpreter_),            "entryPointFromInterpreter"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, entry_point_from_jni_),                    "entryPointFromJni"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, entry_point_from_portable_compiled_code_), "entryPointFromPortableCompiledCode"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, entry_point_from_quick_compiled_code_),    "entryPointFromQuickCompiledCode"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, gc_map_),                                  "gcMap"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, access_flags_),                   "accessFlags"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, dex_code_item_offset_),           "dexCodeItemOffset"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, dex_method_index_),               "dexMethodIndex"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ArtMethod, method_index_),                   "methodIndex"));
  };
};

struct ClassOffsets : public CheckOffsets<mirror::Class> {
  ClassOffsets() : CheckOffsets<mirror::Class>(false, "Ljava/lang/Class;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, class_loader_),                  "classLoader"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, component_type_),                "componentType"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, dex_cache_),                     "dexCache"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, direct_methods_),                "directMethods"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, ifields_),                       "iFields"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, iftable_),                       "ifTable"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, imtable_),                       "imTable"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, name_),                          "name"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, sfields_),                       "sFields"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, super_class_),                   "superClass"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, verify_error_class_),            "verifyErrorClass"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, virtual_methods_),               "virtualMethods"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, vtable_),                        "vtable"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, access_flags_),                  "accessFlags"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, class_size_),                    "classSize"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, clinit_thread_id_),              "clinitThreadId"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, dex_class_def_idx_),             "dexClassDefIndex"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, dex_type_idx_),                  "dexTypeIndex"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, num_reference_instance_fields_), "numReferenceInstanceFields"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, num_reference_static_fields_),   "numReferenceStaticFields"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, object_size_),                   "objectSize"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, primitive_type_),                "primitiveType"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, reference_instance_offsets_),    "referenceInstanceOffsets"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, reference_static_offsets_),      "referenceStaticOffsets"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Class, status_),                        "status"));
  };
};

struct StringOffsets : public CheckOffsets<mirror::String> {
  StringOffsets() : CheckOffsets<mirror::String>(false, "Ljava/lang/String;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::String, array_),     "value"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::String, count_),     "count"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::String, hash_code_), "hashCode"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::String, offset_),    "offset"));
  };
};

struct ThrowableOffsets : public CheckOffsets<mirror::Throwable> {
  ThrowableOffsets() : CheckOffsets<mirror::Throwable>(false, "Ljava/lang/Throwable;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Throwable, cause_),                 "cause"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Throwable, detail_message_),        "detailMessage"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Throwable, stack_state_),           "stackState"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Throwable, stack_trace_),           "stackTrace"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Throwable, suppressed_exceptions_), "suppressedExceptions"));
  };
};

struct StackTraceElementOffsets : public CheckOffsets<mirror::StackTraceElement> {
  StackTraceElementOffsets() : CheckOffsets<mirror::StackTraceElement>(false, "Ljava/lang/StackTraceElement;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::StackTraceElement, declaring_class_), "declaringClass"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::StackTraceElement, file_name_),       "fileName"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::StackTraceElement, method_name_),     "methodName"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::StackTraceElement, line_number_),     "lineNumber"));
  };
};

struct ClassLoaderOffsets : public CheckOffsets<mirror::ClassLoader> {
  ClassLoaderOffsets() : CheckOffsets<mirror::ClassLoader>(false, "Ljava/lang/ClassLoader;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ClassLoader, packages_),   "packages"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ClassLoader, parent_),     "parent"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ClassLoader, proxyCache_), "proxyCache"));
  };
};

struct ProxyOffsets : public CheckOffsets<mirror::Proxy> {
  ProxyOffsets() : CheckOffsets<mirror::Proxy>(false, "Ljava/lang/reflect/Proxy;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Proxy, h_), "h"));
  };
};

struct ClassClassOffsets : public CheckOffsets<mirror::ClassClass> {
  ClassClassOffsets() : CheckOffsets<mirror::ClassClass>(true, "Ljava/lang/Class;") {
    // alphabetical 64-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::ClassClass, serialVersionUID_), "serialVersionUID"));
  };
};

struct StringClassOffsets : public CheckOffsets<mirror::StringClass> {
  StringClassOffsets() : CheckOffsets<mirror::StringClass>(true, "Ljava/lang/String;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::StringClass, ASCII_),                  "ASCII"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::StringClass, CASE_INSENSITIVE_ORDER_), "CASE_INSENSITIVE_ORDER"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::StringClass, REPLACEMENT_CHAR_),       "REPLACEMENT_CHAR"));

    // alphabetical 64-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::StringClass, serialVersionUID_),       "serialVersionUID"));
  };
};

struct ArtFieldClassOffsets : public CheckOffsets<mirror::ArtFieldClass> {
  ArtFieldClassOffsets() : CheckOffsets<mirror::ArtFieldClass>(true, "Ljava/lang/reflect/ArtField;") {
  };
};

struct ArtMethodClassOffsets : public CheckOffsets<mirror::ArtMethodClass> {
  ArtMethodClassOffsets() : CheckOffsets<mirror::ArtMethodClass>(true, "Ljava/lang/reflect/ArtMethod;") {
  };
};

struct DexCacheOffsets : public CheckOffsets<mirror::DexCache> {
  DexCacheOffsets() : CheckOffsets<mirror::DexCache>(false, "Ljava/lang/DexCache;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::DexCache, dex_),                        "dex"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::DexCache, location_),                   "location"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::DexCache, resolved_fields_),            "resolvedFields"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::DexCache, resolved_methods_),           "resolvedMethods"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::DexCache, resolved_types_),             "resolvedTypes"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::DexCache, strings_),                    "strings"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::DexCache, dex_file_),                   "dexFile"));
  };
};

struct ReferenceOffsets : public CheckOffsets<mirror::Reference> {
  ReferenceOffsets() : CheckOffsets<mirror::Reference>(false, "Ljava/lang/ref/Reference;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Reference, pending_next_),  "pendingNext"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Reference, queue_),         "queue"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Reference, queue_next_),    "queueNext"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::Reference, referent_),      "referent"));
  };
};

struct FinalizerReferenceOffsets : public CheckOffsets<mirror::FinalizerReference> {
  FinalizerReferenceOffsets() : CheckOffsets<mirror::FinalizerReference>(false, "Ljava/lang/ref/FinalizerReference;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::FinalizerReference, next_),   "next"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::FinalizerReference, prev_),   "prev"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(mirror::FinalizerReference, zombie_), "zombie"));
  };
};

// C++ fields must exactly match the fields in the Java classes. If this fails,
// reorder the fields in the C++ class. Managed class fields are ordered by
// ClassLinker::LinkFields.
TEST_F(ClassLinkerTest, ValidateFieldOrderOfJavaCppUnionClasses) {
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_TRUE(ObjectOffsets().Check());
  EXPECT_TRUE(ArtFieldOffsets().Check());
  EXPECT_TRUE(ArtMethodOffsets().Check());
  EXPECT_TRUE(ClassOffsets().Check());
  EXPECT_TRUE(StringOffsets().Check());
  EXPECT_TRUE(ThrowableOffsets().Check());
  EXPECT_TRUE(StackTraceElementOffsets().Check());
  EXPECT_TRUE(ClassLoaderOffsets().Check());
  EXPECT_TRUE(ProxyOffsets().Check());
  EXPECT_TRUE(DexCacheOffsets().Check());
  EXPECT_TRUE(ReferenceOffsets().Check());
  EXPECT_TRUE(FinalizerReferenceOffsets().Check());

  EXPECT_TRUE(ClassClassOffsets().Check());
  EXPECT_TRUE(StringClassOffsets().Check());
  EXPECT_TRUE(ArtFieldClassOffsets().Check());
  EXPECT_TRUE(ArtMethodClassOffsets().Check());
}

TEST_F(ClassLinkerTest, FindClassNonexistent) {
  ScopedObjectAccess soa(Thread::Current());
  AssertNonExistentClass("NoSuchClass;");
  AssertNonExistentClass("LNoSuchClass;");
}

TEST_F(ClassLinkerTest, FindClassNested) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Nested"))));

  mirror::Class* outer = class_linker_->FindClass(soa.Self(), "LNested;", class_loader);
  ASSERT_TRUE(outer != NULL);
  EXPECT_EQ(0U, outer->NumVirtualMethods());
  EXPECT_EQ(1U, outer->NumDirectMethods());

  mirror::Class* inner = class_linker_->FindClass(soa.Self(), "LNested$Inner;", class_loader);
  ASSERT_TRUE(inner != NULL);
  EXPECT_EQ(0U, inner->NumVirtualMethods());
  EXPECT_EQ(1U, inner->NumDirectMethods());
}

TEST_F(ClassLinkerTest, FindClass_Primitives) {
  ScopedObjectAccess soa(Thread::Current());
  const std::string expected("BCDFIJSZV");
  for (int ch = 1; ch < 256; ++ch) {
    std::string descriptor;
    descriptor.push_back(ch);
    if (expected.find(ch) == std::string::npos) {
      AssertNonExistentClass(descriptor);
    } else {
      AssertPrimitiveClass(descriptor);
    }
  }
}

TEST_F(ClassLinkerTest, FindClass) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::Class* JavaLangObject = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;");
  ClassHelper kh(JavaLangObject);
  ASSERT_TRUE(JavaLangObject != NULL);
  ASSERT_TRUE(JavaLangObject->GetClass() != NULL);
  ASSERT_EQ(JavaLangObject->GetClass(), JavaLangObject->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, JavaLangObject->GetClass()->GetSuperClass());
  ASSERT_STREQ(kh.GetDescriptor(), "Ljava/lang/Object;");
  EXPECT_TRUE(JavaLangObject->GetSuperClass() == NULL);
  EXPECT_FALSE(JavaLangObject->HasSuperClass());
  EXPECT_TRUE(JavaLangObject->GetClassLoader() == NULL);
  EXPECT_EQ(mirror::Class::kStatusInitialized, JavaLangObject->GetStatus());
  EXPECT_FALSE(JavaLangObject->IsErroneous());
  EXPECT_TRUE(JavaLangObject->IsLoaded());
  EXPECT_TRUE(JavaLangObject->IsResolved());
  EXPECT_TRUE(JavaLangObject->IsVerified());
  EXPECT_TRUE(JavaLangObject->IsInitialized());
  EXPECT_FALSE(JavaLangObject->IsArrayInstance());
  EXPECT_FALSE(JavaLangObject->IsArrayClass());
  EXPECT_TRUE(JavaLangObject->GetComponentType() == NULL);
  EXPECT_FALSE(JavaLangObject->IsInterface());
  EXPECT_TRUE(JavaLangObject->IsPublic());
  EXPECT_FALSE(JavaLangObject->IsFinal());
  EXPECT_FALSE(JavaLangObject->IsPrimitive());
  EXPECT_FALSE(JavaLangObject->IsSynthetic());
  EXPECT_EQ(2U, JavaLangObject->NumDirectMethods());
  EXPECT_EQ(11U, JavaLangObject->NumVirtualMethods());
  if (!kUseBakerOrBrooksReadBarrier) {
    EXPECT_EQ(2U, JavaLangObject->NumInstanceFields());
  } else {
    EXPECT_EQ(4U, JavaLangObject->NumInstanceFields());
  }
  FieldHelper fh(JavaLangObject->GetInstanceField(0));
  EXPECT_STREQ(fh.GetName(), "shadow$_klass_");
  fh.ChangeField(JavaLangObject->GetInstanceField(1));
  EXPECT_STREQ(fh.GetName(), "shadow$_monitor_");
  if (kUseBakerOrBrooksReadBarrier) {
    fh.ChangeField(JavaLangObject->GetInstanceField(2));
    EXPECT_STREQ(fh.GetName(), "shadow$_x_rb_ptr_");
    fh.ChangeField(JavaLangObject->GetInstanceField(3));
    EXPECT_STREQ(fh.GetName(), "shadow$_x_xpadding_");
  }

  EXPECT_EQ(0U, JavaLangObject->NumStaticFields());
  EXPECT_EQ(0U, kh.NumDirectInterfaces());

  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("MyClass"))));
  AssertNonExistentClass("LMyClass;");
  mirror::Class* MyClass = class_linker_->FindClass(soa.Self(), "LMyClass;", class_loader);
  kh.ChangeClass(MyClass);
  ASSERT_TRUE(MyClass != NULL);
  ASSERT_TRUE(MyClass->GetClass() != NULL);
  ASSERT_EQ(MyClass->GetClass(), MyClass->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, MyClass->GetClass()->GetSuperClass());
  ASSERT_STREQ(kh.GetDescriptor(), "LMyClass;");
  EXPECT_TRUE(MyClass->GetSuperClass() == JavaLangObject);
  EXPECT_TRUE(MyClass->HasSuperClass());
  EXPECT_EQ(class_loader.Get(), MyClass->GetClassLoader());
  EXPECT_EQ(mirror::Class::kStatusResolved, MyClass->GetStatus());
  EXPECT_FALSE(MyClass->IsErroneous());
  EXPECT_TRUE(MyClass->IsLoaded());
  EXPECT_TRUE(MyClass->IsResolved());
  EXPECT_FALSE(MyClass->IsVerified());
  EXPECT_FALSE(MyClass->IsInitialized());
  EXPECT_FALSE(MyClass->IsArrayInstance());
  EXPECT_FALSE(MyClass->IsArrayClass());
  EXPECT_TRUE(MyClass->GetComponentType() == NULL);
  EXPECT_FALSE(MyClass->IsInterface());
  EXPECT_FALSE(MyClass->IsPublic());
  EXPECT_FALSE(MyClass->IsFinal());
  EXPECT_FALSE(MyClass->IsPrimitive());
  EXPECT_FALSE(MyClass->IsSynthetic());
  EXPECT_EQ(1U, MyClass->NumDirectMethods());
  EXPECT_EQ(0U, MyClass->NumVirtualMethods());
  EXPECT_EQ(0U, MyClass->NumInstanceFields());
  EXPECT_EQ(0U, MyClass->NumStaticFields());
  EXPECT_EQ(0U, kh.NumDirectInterfaces());

  EXPECT_EQ(JavaLangObject->GetClass()->GetClass(), MyClass->GetClass()->GetClass());

  // created by class_linker
  AssertArrayClass("[C", "C", NULL);
  AssertArrayClass("[Ljava/lang/Object;", "Ljava/lang/Object;", NULL);
  // synthesized on the fly
  AssertArrayClass("[[C", "[C", NULL);
  AssertArrayClass("[[[LMyClass;", "[[LMyClass;", class_loader.Get());
  // or not available at all
  AssertNonExistentClass("[[[[LNonExistentClass;");
}

TEST_F(ClassLinkerTest, LibCore) {
  ScopedObjectAccess soa(Thread::Current());
  AssertDexFile(java_lang_dex_file_, NULL);
}

// The first reference array element must be a multiple of 4 bytes from the
// start of the object
TEST_F(ClassLinkerTest, ValidateObjectArrayElementsOffset) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::Class* array_class = class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/String;");
  mirror::ObjectArray<mirror::String>* array =
      mirror::ObjectArray<mirror::String>::Alloc(soa.Self(), array_class, 0);
  uintptr_t data_offset =
      reinterpret_cast<uintptr_t>(array->GetRawData(sizeof(mirror::HeapReference<mirror::String>),
                                                    0));
  if (sizeof(mirror::HeapReference<mirror::String>) == sizeof(int32_t)) {
    EXPECT_TRUE(IsAligned<4>(data_offset));  // Check 4 byte alignment.
  } else {
    EXPECT_TRUE(IsAligned<8>(data_offset));  // Check 8 byte alignment.
  }
}

TEST_F(ClassLinkerTest, ValidatePrimitiveArrayElementsOffset) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<5> hs(soa.Self());
  Handle<mirror::LongArray> long_array(hs.NewHandle(mirror::LongArray::Alloc(soa.Self(), 0)));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "[J"), long_array->GetClass());
  uintptr_t data_offset = reinterpret_cast<uintptr_t>(long_array->GetData());
  EXPECT_TRUE(IsAligned<8>(data_offset));  // Longs require 8 byte alignment

  Handle<mirror::DoubleArray> double_array(hs.NewHandle(mirror::DoubleArray::Alloc(soa.Self(), 0)));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "[D"), double_array->GetClass());
  data_offset = reinterpret_cast<uintptr_t>(double_array->GetData());
  EXPECT_TRUE(IsAligned<8>(data_offset));  // Doubles require 8 byte alignment

  Handle<mirror::IntArray> int_array(hs.NewHandle(mirror::IntArray::Alloc(soa.Self(), 0)));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "[I"), int_array->GetClass());
  data_offset = reinterpret_cast<uintptr_t>(int_array->GetData());
  EXPECT_TRUE(IsAligned<4>(data_offset));  // Ints require 4 byte alignment

  Handle<mirror::CharArray> char_array(hs.NewHandle(mirror::CharArray::Alloc(soa.Self(), 0)));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "[C"), char_array->GetClass());
  data_offset = reinterpret_cast<uintptr_t>(char_array->GetData());
  EXPECT_TRUE(IsAligned<2>(data_offset));  // Chars require 2 byte alignment

  Handle<mirror::ShortArray> short_array(hs.NewHandle(mirror::ShortArray::Alloc(soa.Self(), 0)));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "[S"), short_array->GetClass());
  data_offset = reinterpret_cast<uintptr_t>(short_array->GetData());
  EXPECT_TRUE(IsAligned<2>(data_offset));  // Shorts require 2 byte alignment

  // Take it as given that bytes and booleans have byte alignment
}

TEST_F(ClassLinkerTest, ValidateBoxedTypes) {
  // Validate that the "value" field is always the 0th field in each of java.lang's box classes.
  // This lets UnboxPrimitive avoid searching for the field by name at runtime.
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  auto class_loader(hs.NewHandle<mirror::ClassLoader>(nullptr));
  mirror::Class* c;
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Boolean;", class_loader);
  FieldHelper fh(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Byte;", class_loader);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Character;", class_loader);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Double;", class_loader);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Float;", class_loader);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Integer;", class_loader);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Long;", class_loader);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass(soa.Self(), "Ljava/lang/Short;", class_loader);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
}

TEST_F(ClassLinkerTest, TwoClassLoadersOneClass) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader_1(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("MyClass"))));
  Handle<mirror::ClassLoader> class_loader_2(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("MyClass"))));
  mirror::Class* MyClass_1 = class_linker_->FindClass(soa.Self(), "LMyClass;", class_loader_1);
  mirror::Class* MyClass_2 = class_linker_->FindClass(soa.Self(), "LMyClass;", class_loader_2);
  EXPECT_TRUE(MyClass_1 != nullptr);
  EXPECT_TRUE(MyClass_2 != nullptr);
  EXPECT_NE(MyClass_1, MyClass_2);
}

TEST_F(ClassLinkerTest, StaticFields) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Statics"))));
  Handle<mirror::Class> statics(
      hs.NewHandle(class_linker_->FindClass(soa.Self(), "LStatics;", class_loader)));
  class_linker_->EnsureInitialized(statics, true, true);

  // Static final primitives that are initialized by a compile-time constant
  // expression resolve to a copy of a constant value from the constant pool.
  // So <clinit> should be null.
  mirror::ArtMethod* clinit = statics->FindDirectMethod("<clinit>", "()V");
  EXPECT_TRUE(clinit == NULL);

  EXPECT_EQ(9U, statics->NumStaticFields());

  mirror::ArtField* s0 = statics->FindStaticField("s0", "Z");
  FieldHelper fh(s0);
  EXPECT_STREQ(ClassHelper(s0->GetClass()).GetDescriptor(), "Ljava/lang/reflect/ArtField;");
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimBoolean);
  EXPECT_EQ(true, s0->GetBoolean(statics.Get()));
  s0->SetBoolean<false>(statics.Get(), false);

  mirror::ArtField* s1 = statics->FindStaticField("s1", "B");
  fh.ChangeField(s1);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimByte);
  EXPECT_EQ(5, s1->GetByte(statics.Get()));
  s1->SetByte<false>(statics.Get(), 6);

  mirror::ArtField* s2 = statics->FindStaticField("s2", "C");
  fh.ChangeField(s2);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimChar);
  EXPECT_EQ('a', s2->GetChar(statics.Get()));
  s2->SetChar<false>(statics.Get(), 'b');

  mirror::ArtField* s3 = statics->FindStaticField("s3", "S");
  fh.ChangeField(s3);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimShort);
  EXPECT_EQ(-536, s3->GetShort(statics.Get()));
  s3->SetShort<false>(statics.Get(), -535);

  mirror::ArtField* s4 = statics->FindStaticField("s4", "I");
  fh.ChangeField(s4);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimInt);
  EXPECT_EQ(2000000000, s4->GetInt(statics.Get()));
  s4->SetInt<false>(statics.Get(), 2000000001);

  mirror::ArtField* s5 = statics->FindStaticField("s5", "J");
  fh.ChangeField(s5);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimLong);
  EXPECT_EQ(0x1234567890abcdefLL, s5->GetLong(statics.Get()));
  s5->SetLong<false>(statics.Get(), INT64_C(0x34567890abcdef12));

  mirror::ArtField* s6 = statics->FindStaticField("s6", "F");
  fh.ChangeField(s6);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimFloat);
  EXPECT_EQ(0.5, s6->GetFloat(statics.Get()));
  s6->SetFloat<false>(statics.Get(), 0.75);

  mirror::ArtField* s7 = statics->FindStaticField("s7", "D");
  fh.ChangeField(s7);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimDouble);
  EXPECT_EQ(16777217, s7->GetDouble(statics.Get()));
  s7->SetDouble<false>(statics.Get(), 16777219);

  mirror::ArtField* s8 = statics->FindStaticField("s8", "Ljava/lang/String;");
  fh.ChangeField(s8);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimNot);
  EXPECT_TRUE(s8->GetObject(statics.Get())->AsString()->Equals("android"));
  s8->SetObject<false>(s8->GetDeclaringClass(),
                       mirror::String::AllocFromModifiedUtf8(soa.Self(), "robot"));

  // TODO: Remove EXPECT_FALSE when GCC can handle EXPECT_EQ
  // http://code.google.com/p/googletest/issues/detail?id=322
  EXPECT_FALSE(s0->GetBoolean(statics.Get()));
  EXPECT_EQ(6, s1->GetByte(statics.Get()));
  EXPECT_EQ('b', s2->GetChar(statics.Get()));
  EXPECT_EQ(-535, s3->GetShort(statics.Get()));
  EXPECT_EQ(2000000001, s4->GetInt(statics.Get()));
  EXPECT_EQ(INT64_C(0x34567890abcdef12), s5->GetLong(statics.Get()));
  EXPECT_EQ(0.75, s6->GetFloat(statics.Get()));
  EXPECT_EQ(16777219, s7->GetDouble(statics.Get()));
  EXPECT_TRUE(s8->GetObject(statics.Get())->AsString()->Equals("robot"));
}

TEST_F(ClassLinkerTest, Interfaces) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(LoadDex("Interfaces"))));
  mirror::Class* I = class_linker_->FindClass(soa.Self(), "LInterfaces$I;", class_loader);
  mirror::Class* J = class_linker_->FindClass(soa.Self(), "LInterfaces$J;", class_loader);
  mirror::Class* K = class_linker_->FindClass(soa.Self(), "LInterfaces$K;", class_loader);
  mirror::Class* A = class_linker_->FindClass(soa.Self(), "LInterfaces$A;", class_loader);
  mirror::Class* B = class_linker_->FindClass(soa.Self(), "LInterfaces$B;", class_loader);
  EXPECT_TRUE(I->IsAssignableFrom(A));
  EXPECT_TRUE(J->IsAssignableFrom(A));
  EXPECT_TRUE(J->IsAssignableFrom(K));
  EXPECT_TRUE(K->IsAssignableFrom(B));
  EXPECT_TRUE(J->IsAssignableFrom(B));

  const Signature void_sig = I->GetDexCache()->GetDexFile()->CreateSignature("()V");
  mirror::ArtMethod* Ii = I->FindVirtualMethod("i", void_sig);
  mirror::ArtMethod* Jj1 = J->FindVirtualMethod("j1", void_sig);
  mirror::ArtMethod* Jj2 = J->FindVirtualMethod("j2", void_sig);
  mirror::ArtMethod* Kj1 = K->FindInterfaceMethod("j1", void_sig);
  mirror::ArtMethod* Kj2 = K->FindInterfaceMethod("j2", void_sig);
  mirror::ArtMethod* Kk = K->FindInterfaceMethod("k", void_sig);
  mirror::ArtMethod* Ai = A->FindVirtualMethod("i", void_sig);
  mirror::ArtMethod* Aj1 = A->FindVirtualMethod("j1", void_sig);
  mirror::ArtMethod* Aj2 = A->FindVirtualMethod("j2", void_sig);
  ASSERT_TRUE(Ii != NULL);
  ASSERT_TRUE(Jj1 != NULL);
  ASSERT_TRUE(Jj2 != NULL);
  ASSERT_TRUE(Kj1 != NULL);
  ASSERT_TRUE(Kj2 != NULL);
  ASSERT_TRUE(Kk != NULL);
  ASSERT_TRUE(Ai != NULL);
  ASSERT_TRUE(Aj1 != NULL);
  ASSERT_TRUE(Aj2 != NULL);
  EXPECT_NE(Ii, Ai);
  EXPECT_NE(Jj1, Aj1);
  EXPECT_NE(Jj2, Aj2);
  EXPECT_EQ(Kj1, Jj1);
  EXPECT_EQ(Kj2, Jj2);
  EXPECT_EQ(Ai, A->FindVirtualMethodForInterface(Ii));
  EXPECT_EQ(Aj1, A->FindVirtualMethodForInterface(Jj1));
  EXPECT_EQ(Aj2, A->FindVirtualMethodForInterface(Jj2));
  EXPECT_EQ(Ai, A->FindVirtualMethodForVirtualOrInterface(Ii));
  EXPECT_EQ(Aj1, A->FindVirtualMethodForVirtualOrInterface(Jj1));
  EXPECT_EQ(Aj2, A->FindVirtualMethodForVirtualOrInterface(Jj2));

  mirror::ArtField* Afoo = A->FindStaticField("foo", "Ljava/lang/String;");
  mirror::ArtField* Bfoo = B->FindStaticField("foo", "Ljava/lang/String;");
  mirror::ArtField* Jfoo = J->FindStaticField("foo", "Ljava/lang/String;");
  mirror::ArtField* Kfoo = K->FindStaticField("foo", "Ljava/lang/String;");
  ASSERT_TRUE(Afoo != NULL);
  EXPECT_EQ(Afoo, Bfoo);
  EXPECT_EQ(Afoo, Jfoo);
  EXPECT_EQ(Afoo, Kfoo);
}

TEST_F(ClassLinkerTest, ResolveVerifyAndClinit) {
  // pretend we are trying to get the static storage for the StaticsFromCode class.

  // case 1, get the uninitialized storage from StaticsFromCode.<clinit>
  // case 2, get the initialized storage from StaticsFromCode.getS0

  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("StaticsFromCode");
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
  const DexFile* dex_file = Runtime::Current()->GetCompileTimeClassPath(jclass_loader)[0];
  CHECK(dex_file != NULL);
  mirror::Class* klass = class_linker_->FindClass(soa.Self(), "LStaticsFromCode;", class_loader);
  mirror::ArtMethod* clinit = klass->FindClassInitializer();
  mirror::ArtMethod* getS0 = klass->FindDirectMethod("getS0", "()Ljava/lang/Object;");
  const DexFile::StringId* string_id = dex_file->FindStringId("LStaticsFromCode;");
  ASSERT_TRUE(string_id != NULL);
  const DexFile::TypeId* type_id = dex_file->FindTypeId(dex_file->GetIndexForStringId(*string_id));
  ASSERT_TRUE(type_id != NULL);
  uint32_t type_idx = dex_file->GetIndexForTypeId(*type_id);
  mirror::Class* uninit = ResolveVerifyAndClinit(type_idx, clinit, Thread::Current(), true, false);
  EXPECT_TRUE(uninit != NULL);
  EXPECT_FALSE(uninit->IsInitialized());
  mirror::Class* init = ResolveVerifyAndClinit(type_idx, getS0, Thread::Current(), true, false);
  EXPECT_TRUE(init != NULL);
  EXPECT_TRUE(init->IsInitialized());
}

TEST_F(ClassLinkerTest, FinalizableBit) {
  ScopedObjectAccess soa(Thread::Current());
  mirror::Class* c;

  // Object has a finalize method, but we know it's empty.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;");
  EXPECT_FALSE(c->IsFinalizable());

  // Enum has a finalize method to prevent its subclasses from implementing one.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Enum;");
  EXPECT_FALSE(c->IsFinalizable());

  // RoundingMode is an enum.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/math/RoundingMode;");
  EXPECT_FALSE(c->IsFinalizable());

  // RandomAccessFile extends Object and overrides finalize.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/io/RandomAccessFile;");
  EXPECT_TRUE(c->IsFinalizable());

  // FileInputStream is finalizable and extends InputStream which isn't.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/io/InputStream;");
  EXPECT_FALSE(c->IsFinalizable());
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/io/FileInputStream;");
  EXPECT_TRUE(c->IsFinalizable());

  // ScheduledThreadPoolExecutor doesn't have a finalize method but
  // extends ThreadPoolExecutor which does.
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/util/concurrent/ThreadPoolExecutor;");
  EXPECT_TRUE(c->IsFinalizable());
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/util/concurrent/ScheduledThreadPoolExecutor;");
  EXPECT_TRUE(c->IsFinalizable());
}

TEST_F(ClassLinkerTest, ClassRootDescriptors) {
  ScopedObjectAccess soa(Thread::Current());
  for (int i = 0; i < ClassLinker::kClassRootsMax; i++) {
    mirror::Class* klass = class_linker_->GetClassRoot(ClassLinker::ClassRoot(i));
    ClassHelper kh(klass);
    EXPECT_TRUE(kh.GetDescriptor() != NULL);
    EXPECT_STREQ(kh.GetDescriptor(),
                 class_linker_->GetClassRootDescriptor(ClassLinker::ClassRoot(i))) << " i = " << i;
  }
}

}  // namespace art
