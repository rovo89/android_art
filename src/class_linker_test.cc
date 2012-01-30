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
#include "common_test.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "heap.h"
#include "runtime_support.h"

namespace art {

class ClassLinkerTest : public CommonTest {
 protected:
  void AssertNonExistentClass(const std::string& descriptor) {
    EXPECT_TRUE(class_linker_->FindSystemClass(descriptor.c_str()) == NULL);
    Thread* self = Thread::Current();
    EXPECT_TRUE(self->IsExceptionPending());
    Object* exception = self->GetException();
    self->ClearException();
    Class* exception_class = class_linker_->FindSystemClass("Ljava/lang/NoClassDefFoundError;");
    EXPECT_TRUE(exception->InstanceOf(exception_class));
  }

  void AssertPrimitiveClass(const std::string& descriptor) {
    AssertPrimitiveClass(descriptor, class_linker_->FindSystemClass(descriptor.c_str()));
  }

  void AssertPrimitiveClass(const std::string& descriptor, const Class* primitive) {
    ClassHelper primitive_ch(primitive);
    ASSERT_TRUE(primitive != NULL);
    ASSERT_TRUE(primitive->GetClass() != NULL);
    ASSERT_EQ(primitive->GetClass(), primitive->GetClass()->GetClass());
    EXPECT_TRUE(primitive->GetClass()->GetSuperClass() != NULL);
    ASSERT_STREQ(descriptor.c_str(), primitive_ch.GetDescriptor());
    EXPECT_TRUE(primitive->GetSuperClass() == NULL);
    EXPECT_FALSE(primitive->HasSuperClass());
    EXPECT_TRUE(primitive->GetClassLoader() == NULL);
    EXPECT_EQ(Class::kStatusInitialized, primitive->GetStatus());
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
    EXPECT_EQ(0U, primitive_ch.NumInterfaces());
    EXPECT_TRUE(primitive->GetVTable() == NULL);
    EXPECT_EQ(0, primitive->GetIfTableCount());
    EXPECT_TRUE(primitive->GetIfTable() == NULL);
  }

  void AssertArrayClass(const std::string& array_descriptor,
                        const std::string& component_type,
                        const ClassLoader* class_loader) {
    Class* array = class_linker_->FindClass(array_descriptor.c_str(), class_loader);
    ClassHelper array_component_ch(array->GetComponentType());
    EXPECT_STREQ(component_type.c_str(), array_component_ch.GetDescriptor());
    EXPECT_EQ(class_loader, array->GetClassLoader());
    AssertArrayClass(array_descriptor, array);
  }

  void AssertArrayClass(const std::string& array_descriptor, Class* array) {
    ClassHelper kh(array);
    ASSERT_TRUE(array != NULL);
    ASSERT_TRUE(array->GetClass() != NULL);
    ASSERT_EQ(array->GetClass(), array->GetClass()->GetClass());
    EXPECT_TRUE(array->GetClass()->GetSuperClass() != NULL);
    ASSERT_STREQ(array_descriptor.c_str(), kh.GetDescriptor());
    EXPECT_TRUE(array->GetSuperClass() != NULL);
    EXPECT_EQ(class_linker_->FindSystemClass("Ljava/lang/Object;"), array->GetSuperClass());
    EXPECT_TRUE(array->HasSuperClass());
    ASSERT_TRUE(array->GetComponentType() != NULL);
    kh.ChangeClass(array->GetComponentType());
    ASSERT_TRUE(kh.GetDescriptor() != NULL);
    EXPECT_EQ(Class::kStatusInitialized, array->GetStatus());
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
    kh.ChangeClass(array);
    EXPECT_EQ(2U, kh.NumInterfaces());
    EXPECT_TRUE(array->GetVTable() != NULL);
    EXPECT_EQ(2, array->GetIfTableCount());
    ObjectArray<InterfaceEntry>* iftable = array->GetIfTable();
    ASSERT_TRUE(iftable != NULL);
    kh.ChangeClass(kh.GetInterface(0));
    EXPECT_STREQ(kh.GetDescriptor(), "Ljava/lang/Cloneable;");
    kh.ChangeClass(array);
    kh.ChangeClass(kh.GetInterface(1));
    EXPECT_STREQ(kh.GetDescriptor(), "Ljava/io/Serializable;");
  }

  void AssertMethod(Class* klass, Method* method) {
    MethodHelper mh(method);
    EXPECT_TRUE(method != NULL);
    EXPECT_TRUE(method->GetClass() != NULL);
    EXPECT_TRUE(mh.GetName() != NULL);
    EXPECT_TRUE(mh.GetSignature() != NULL);

    EXPECT_TRUE(method->GetDexCacheStrings() != NULL);
    EXPECT_TRUE(method->GetDexCacheResolvedTypes() != NULL);
    EXPECT_TRUE(method->GetDexCacheResolvedMethods() != NULL);
    EXPECT_TRUE(method->GetDexCacheResolvedFields() != NULL);
    EXPECT_TRUE(method->GetDexCacheCodeAndDirectMethods() != NULL);
    EXPECT_TRUE(method->GetDexCacheInitializedStaticStorage() != NULL);
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetStrings(),
              method->GetDexCacheStrings());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetResolvedTypes(),
              method->GetDexCacheResolvedTypes());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetResolvedMethods(),
              method->GetDexCacheResolvedMethods());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetResolvedFields(),
              method->GetDexCacheResolvedFields());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetCodeAndDirectMethods(),
              method->GetDexCacheCodeAndDirectMethods());
    EXPECT_EQ(method->GetDeclaringClass()->GetDexCache()->GetInitializedStaticStorage(),
              method->GetDexCacheInitializedStaticStorage());
  }

  void AssertField(Class* klass, Field* field) {
    FieldHelper fh(field);
    EXPECT_TRUE(field != NULL);
    EXPECT_TRUE(field->GetClass() != NULL);
    EXPECT_EQ(klass, field->GetDeclaringClass());
    EXPECT_TRUE(fh.GetName() != NULL);
    EXPECT_TRUE(fh.GetType() != NULL);
  }

  void AssertClass(const std::string& descriptor, Class* klass) {
    ClassHelper kh(klass);
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
    EXPECT_TRUE(klass->IsInSamePackage(klass));
    EXPECT_TRUE(Class::IsInSamePackage(kh.GetDescriptor(), kh.GetDescriptor()));
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
    for (int i = 0; i < klass->GetIfTableCount(); i++) {
      const InterfaceEntry* interface_entry = klass->GetIfTable()->Get(i);
      ASSERT_TRUE(interface_entry != NULL);
      Class* interface = interface_entry->GetInterface();
      ASSERT_TRUE(interface != NULL);
      EXPECT_TRUE(interface_entry->GetInterface() != NULL);
      if (klass->IsInterface()) {
        EXPECT_EQ(0U, interface_entry->GetMethodArrayCount());
      } else {
        CHECK_EQ(interface->NumVirtualMethods(), interface_entry->GetMethodArrayCount());
        EXPECT_EQ(interface->NumVirtualMethods(), interface_entry->GetMethodArrayCount());
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
    EXPECT_TRUE(klass->CanAccess(klass));

    for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
      Method* method = klass->GetDirectMethod(i);
      AssertMethod(klass, method);
      EXPECT_TRUE(method->IsDirect());
      EXPECT_EQ(klass, method->GetDeclaringClass());
    }

    for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
      Method* method = klass->GetVirtualMethod(i);
      AssertMethod(klass, method);
      EXPECT_FALSE(method->IsDirect());
      EXPECT_TRUE(method->GetDeclaringClass()->IsAssignableFrom(klass));
    }

    for (size_t i = 0; i < klass->NumInstanceFields(); i++) {
      Field* field = klass->GetInstanceField(i);
      AssertField(klass, field);
      EXPECT_FALSE(field->IsStatic());
    }

    for (size_t i = 0; i < klass->NumStaticFields(); i++) {
      Field* field = klass->GetStaticField(i);
      AssertField(klass, field);
      EXPECT_TRUE(field->IsStatic());
    }

    // Confirm that all instances fields are packed together at the start
    EXPECT_GE(klass->NumInstanceFields(), klass->NumReferenceInstanceFields());
    FieldHelper fh;
    for (size_t i = 0; i < klass->NumReferenceInstanceFields(); i++) {
      Field* field = klass->GetInstanceField(i);
      fh.ChangeField(field);
      ASSERT_TRUE(!fh.IsPrimitiveType());
      Class* field_type = fh.GetType();
      ASSERT_TRUE(field_type != NULL);
      ASSERT_TRUE(!field_type->IsPrimitive());
    }
    for (size_t i = klass->NumReferenceInstanceFields(); i < klass->NumInstanceFields(); i++) {
      Field* field = klass->GetInstanceField(i);
      fh.ChangeField(field);
      Class* field_type = fh.GetType();
      ASSERT_TRUE(field_type != NULL);
      if (!fh.IsPrimitiveType() || !field_type->IsPrimitive()) {
        // While Reference.referent is not primitive, the ClassLinker
        // treats it as such so that the garbage collector won't scan it.
        EXPECT_EQ(PrettyField(field), "java.lang.Object java.lang.ref.Reference.referent");
      }
    }

    size_t total_num_reference_instance_fields = 0;
    Class* k = klass;
    while (k != NULL) {
      total_num_reference_instance_fields += k->NumReferenceInstanceFields();
      k = k->GetSuperClass();
    }
    EXPECT_EQ(klass->GetReferenceInstanceOffsets() == 0,
              total_num_reference_instance_fields == 0);
  }

  void AssertDexFileClass(ClassLoader* class_loader, const std::string& descriptor) {
    ASSERT_TRUE(descriptor != NULL);
    Class* klass = class_linker_->FindSystemClass(descriptor.c_str());
    ASSERT_TRUE(klass != NULL);
    EXPECT_STREQ(descriptor.c_str(), ClassHelper(klass).GetDescriptor());
    EXPECT_EQ(class_loader, klass->GetClassLoader());
    if (klass->IsPrimitive()) {
      AssertPrimitiveClass(descriptor, klass);
    } else if (klass->IsArrayClass()) {
      AssertArrayClass(descriptor, klass);
    } else {
      AssertClass(descriptor, klass);
    }
  }

  void AssertDexFile(const DexFile* dex, ClassLoader* class_loader) {
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
    class_linker_->VisitRoots(TestRootVisitor, NULL);
  }

  static void TestRootVisitor(const Object* root, void* arg) {
    EXPECT_TRUE(root != NULL);
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

  bool Check() {
    Class* klass = Runtime::Current()->GetClassLinker()->FindSystemClass(class_descriptor.c_str());
    CHECK(klass != NULL) << class_descriptor;

    bool error = false;

    if (!klass->IsClassClass() && !is_static) {
      size_t expected_size = is_static ? klass->GetClassSize(): klass->GetObjectSize();
      if (sizeof(T) != expected_size) {
        LG << "Class size mismatch:"
           << " class=" << class_descriptor
           << " Java=" << expected_size
           << " C++=" << sizeof(T);
        error = true;
      }
    }

    size_t num_fields = is_static ? klass->NumStaticFields() : klass->NumInstanceFields();
    if (offsets.size() != num_fields) {
      LG << "Field count mismatch:"
         << " class=" << class_descriptor
         << " Java=" << num_fields
         << " C++=" << offsets.size();
      error = true;
    }

    FieldHelper fh;
    for (size_t i = 0; i < offsets.size(); i++) {
      Field* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
      fh.ChangeField(field);
      StringPiece field_name(fh.GetName());
      if (field_name != offsets[i].java_name) {
        error = true;
      }
    }
    if (error) {
      for (size_t i = 0; i < offsets.size(); i++) {
        CheckOffset& offset = offsets[i];
        Field* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
        fh.ChangeField(field);
        StringPiece field_name(fh.GetName());
        if (field_name != offsets[i].java_name) {
          LG << "JAVA FIELD ORDER MISMATCH NEXT LINE:";
        }
        LG << "Java field order:"
           << " i=" << i << " class=" << class_descriptor
           << " Java=" << field_name
           << " CheckOffsets=" << offset.java_name;
      }
    }

    for (size_t i = 0; i < offsets.size(); i++) {
      CheckOffset& offset = offsets[i];
      Field* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
      if (field->GetOffset().Uint32Value() != offset.cpp_offset) {
        error = true;
      }
    }
    if (error) {
      for (size_t i = 0; i < offsets.size(); i++) {
        CheckOffset& offset = offsets[i];
        Field* field = is_static ? klass->GetStaticField(i) : klass->GetInstanceField(i);
        if (field->GetOffset().Uint32Value() != offset.cpp_offset) {
          LG << "OFFSET MISMATCH NEXT LINE:";
        }
        LG << "Offset: class=" << class_descriptor << " field=" << offset.java_name
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

struct ObjectOffsets : public CheckOffsets<Object> {
  ObjectOffsets() : CheckOffsets<Object>(false, "Ljava/lang/Object;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Object, klass_),   "shadow$_klass_"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Object, monitor_), "shadow$_monitor_"));
  };
};

struct FieldOffsets : public CheckOffsets<Field> {
  FieldOffsets() : CheckOffsets<Field>(false, "Ljava/lang/reflect/Field;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, declaring_class_), "declaringClass"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, access_flags_),    "accessFlags"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, field_dex_idx_),   "fieldDexIndex"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, offset_),          "offset"));
  };
};

struct MethodOffsets : public CheckOffsets<Method> {
  MethodOffsets() : CheckOffsets<Method>(false, "Ljava/lang/reflect/Method;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, declaring_class_),                      "declaringClass"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_code_and_direct_methods_),    "dexCacheCodeAndDirectMethods"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_initialized_static_storage_), "dexCacheInitializedStaticStorage"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_resolved_fields_),            "dexCacheResolvedFields"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_resolved_methods_),           "dexCacheResolvedMethods"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_resolved_types_),             "dexCacheResolvedTypes"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_strings_),                    "dexCacheStrings"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, access_flags_),        "accessFlags"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, code_),                "code"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, code_item_offset_),    "codeItemOffset"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, core_spill_mask_),     "coreSpillMask"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, fp_spill_mask_),       "fpSpillMask"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, frame_size_in_bytes_), "frameSizeInBytes"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, gc_map_),              "gcMap"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, invoke_stub_),         "invokeStub"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, mapping_table_),       "mappingTable"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, method_dex_index_),    "methodDexIndex"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, method_index_),        "methodIndex"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, native_method_),       "nativeMethod"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, vmap_table_),          "vmapTable"));
  };
};

struct ConstructorOffsets : public MethodOffsets {
  ConstructorOffsets() : MethodOffsets() {
    // We use Method* for both java.lang.reflect.Constructor and java.lang.reflect.Method.
    class_descriptor = "Ljava/lang/reflect/Constructor;";
  }
};

struct ClassOffsets : public CheckOffsets<Class> {
  ClassOffsets() : CheckOffsets<Class>(false, "Ljava/lang/Class;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, class_loader_),                  "classLoader"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, component_type_),                "componentType"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, dex_cache_),                     "dexCache"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, direct_methods_),                "directMethods"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, ifields_),                       "iFields"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, iftable_),                       "ifTable"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, name_),                          "name"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, sfields_),                       "sFields"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, super_class_),                   "superClass"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, verify_error_class_),            "verifyErrorClass"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, virtual_methods_),               "virtualMethods"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, vtable_),                        "vtable"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, access_flags_),                  "accessFlags"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, class_size_),                    "classSize"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, clinit_thread_id_),              "clinitThreadId"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, dex_type_idx_),                  "dexTypeIndex"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, num_reference_instance_fields_), "numReferenceInstanceFields"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, num_reference_static_fields_),   "numReferenceStaticFields"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, object_size_),                   "objectSize"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, primitive_type_),                "primitiveType"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, reference_instance_offsets_),    "referenceInstanceOffsets"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, reference_static_offsets_),      "referenceStaticOffsets"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, status_),                        "status"));
  };
};

struct StringOffsets : public CheckOffsets<String> {
  StringOffsets() : CheckOffsets<String>(false, "Ljava/lang/String;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(String, array_),     "value"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(String, count_),     "count"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(String, hash_code_), "hashCode"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(String, offset_),    "offset"));
  };
};

struct ThrowableOffsets : public CheckOffsets<Throwable> {
  ThrowableOffsets() : CheckOffsets<Throwable>(false, "Ljava/lang/Throwable;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Throwable, cause_),                 "cause"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Throwable, detail_message_),        "detailMessage"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Throwable, stack_state_),           "stackState"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Throwable, stack_trace_),           "stackTrace"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Throwable, suppressed_exceptions_), "suppressedExceptions"));
  };
};

struct StackTraceElementOffsets : public CheckOffsets<StackTraceElement> {
  StackTraceElementOffsets() : CheckOffsets<StackTraceElement>(false, "Ljava/lang/StackTraceElement;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StackTraceElement, declaring_class_), "declaringClass"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StackTraceElement, file_name_),       "fileName"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StackTraceElement, method_name_),     "methodName"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StackTraceElement, line_number_),     "lineNumber"));
  };
};

struct ClassLoaderOffsets : public CheckOffsets<ClassLoader> {
  ClassLoaderOffsets() : CheckOffsets<ClassLoader>(false, "Ljava/lang/ClassLoader;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(ClassLoader, packages_), "packages"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(ClassLoader, parent_),   "parent"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(ClassLoader, proxyCache_),   "proxyCache"));
  };
};

struct BaseDexClassLoaderOffsets : public CheckOffsets<BaseDexClassLoader> {
  BaseDexClassLoaderOffsets()
    : CheckOffsets<BaseDexClassLoader>(false, "Ldalvik/system/BaseDexClassLoader;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(BaseDexClassLoader, original_path_), "originalPath"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(BaseDexClassLoader, path_list_),     "pathList"));
  };
};

struct PathClassLoaderOffsets : public CheckOffsets<PathClassLoader> {
  PathClassLoaderOffsets()
    : CheckOffsets<PathClassLoader>(false, "Ldalvik/system/PathClassLoader;") {}
};

struct ProxyOffsets : public CheckOffsets<Proxy> {
  ProxyOffsets() : CheckOffsets<Proxy>(false, "Ljava/lang/reflect/Proxy;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Proxy, h_), "h"));
  };
};

struct ClassClassOffsets : public CheckOffsets<ClassClass> {
  ClassClassOffsets() : CheckOffsets<ClassClass>(true, "Ljava/lang/Class;") {
    // padding 32-bit
    CHECK_EQ(OFFSETOF_MEMBER(ClassClass, padding_) + 4,
             OFFSETOF_MEMBER(ClassClass, serialVersionUID_));

    // alphabetical 64-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(ClassClass, serialVersionUID_), "serialVersionUID"));
  };
};

struct StringClassOffsets : public CheckOffsets<StringClass> {
  StringClassOffsets() : CheckOffsets<StringClass>(true, "Ljava/lang/String;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StringClass, ASCII_),                  "ASCII"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StringClass, CASE_INSENSITIVE_ORDER_), "CASE_INSENSITIVE_ORDER"));

    // padding 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StringClass, REPLACEMENT_CHAR_),       "REPLACEMENT_CHAR"));

    // alphabetical 64-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StringClass, serialVersionUID_),       "serialVersionUID"));
  };
};

struct FieldClassOffsets : public CheckOffsets<FieldClass> {
  FieldClassOffsets() : CheckOffsets<FieldClass>(true, "Ljava/lang/reflect/Field;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(FieldClass, ORDER_BY_NAME_AND_DECLARING_CLASS_), "ORDER_BY_NAME_AND_DECLARING_CLASS"));
  };
};

struct MethodClassOffsets : public CheckOffsets<MethodClass> {
  MethodClassOffsets() : CheckOffsets<MethodClass>(true, "Ljava/lang/reflect/Method;") {
    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(MethodClass, ORDER_BY_SIGNATURE_), "ORDER_BY_SIGNATURE"));
  };
};

// C++ fields must exactly match the fields in the Java classes. If this fails,
// reorder the fields in the C++ class. Managed class fields are ordered by
// ClassLinker::LinkFields.
TEST_F(ClassLinkerTest, ValidateFieldOrderOfJavaCppUnionClasses) {
  EXPECT_TRUE(ObjectOffsets().Check());
  EXPECT_TRUE(ConstructorOffsets().Check());
  EXPECT_TRUE(FieldOffsets().Check());
  EXPECT_TRUE(MethodOffsets().Check());
  EXPECT_TRUE(ClassOffsets().Check());
  EXPECT_TRUE(StringOffsets().Check());
  EXPECT_TRUE(ThrowableOffsets().Check());
  EXPECT_TRUE(StackTraceElementOffsets().Check());
  EXPECT_TRUE(ClassLoaderOffsets().Check());
  EXPECT_TRUE(BaseDexClassLoaderOffsets().Check());
  EXPECT_TRUE(PathClassLoaderOffsets().Check());
  EXPECT_TRUE(ProxyOffsets().Check());

  EXPECT_TRUE(ClassClassOffsets().Check());
  EXPECT_TRUE(StringClassOffsets().Check());
  EXPECT_TRUE(FieldClassOffsets().Check());
  EXPECT_TRUE(MethodClassOffsets().Check());
}

TEST_F(ClassLinkerTest, FindClassNonexistent) {
  AssertNonExistentClass("NoSuchClass;");
  AssertNonExistentClass("LNoSuchClass;");
}

TEST_F(ClassLinkerTest, FindClassNested) {
  SirtRef<ClassLoader> class_loader(LoadDex("Nested"));

  Class* outer = class_linker_->FindClass("LNested;", class_loader.get());
  ASSERT_TRUE(outer != NULL);
  EXPECT_EQ(0U, outer->NumVirtualMethods());
  EXPECT_EQ(1U, outer->NumDirectMethods());

  Class* inner = class_linker_->FindClass("LNested$Inner;", class_loader.get());
  ASSERT_TRUE(inner != NULL);
  EXPECT_EQ(0U, inner->NumVirtualMethods());
  EXPECT_EQ(1U, inner->NumDirectMethods());
}

TEST_F(ClassLinkerTest, FindClass_Primitives) {
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
  Class* JavaLangObject = class_linker_->FindSystemClass("Ljava/lang/Object;");
  ClassHelper kh(JavaLangObject);
  ASSERT_TRUE(JavaLangObject != NULL);
  ASSERT_TRUE(JavaLangObject->GetClass() != NULL);
  ASSERT_EQ(JavaLangObject->GetClass(), JavaLangObject->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, JavaLangObject->GetClass()->GetSuperClass());
  ASSERT_STREQ(kh.GetDescriptor(), "Ljava/lang/Object;");
  EXPECT_TRUE(JavaLangObject->GetSuperClass() == NULL);
  EXPECT_FALSE(JavaLangObject->HasSuperClass());
  EXPECT_TRUE(JavaLangObject->GetClassLoader() == NULL);
  EXPECT_EQ(Class::kStatusResolved, JavaLangObject->GetStatus());
  EXPECT_FALSE(JavaLangObject->IsErroneous());
  EXPECT_TRUE(JavaLangObject->IsLoaded());
  EXPECT_TRUE(JavaLangObject->IsResolved());
  EXPECT_FALSE(JavaLangObject->IsVerified());
  EXPECT_FALSE(JavaLangObject->IsInitialized());
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
  EXPECT_EQ(2U, JavaLangObject->NumInstanceFields());
  FieldHelper fh(JavaLangObject->GetInstanceField(0));
  EXPECT_STREQ(fh.GetName(), "shadow$_klass_");
  fh.ChangeField(JavaLangObject->GetInstanceField(1));
  EXPECT_STREQ(fh.GetName(), "shadow$_monitor_");

  EXPECT_EQ(0U, JavaLangObject->NumStaticFields());
  EXPECT_EQ(0U, kh.NumInterfaces());

  SirtRef<ClassLoader> class_loader(LoadDex("MyClass"));
  AssertNonExistentClass("LMyClass;");
  Class* MyClass = class_linker_->FindClass("LMyClass;", class_loader.get());
  kh.ChangeClass(MyClass);
  ASSERT_TRUE(MyClass != NULL);
  ASSERT_TRUE(MyClass->GetClass() != NULL);
  ASSERT_EQ(MyClass->GetClass(), MyClass->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, MyClass->GetClass()->GetSuperClass());
  ASSERT_STREQ(kh.GetDescriptor(), "LMyClass;");
  EXPECT_TRUE(MyClass->GetSuperClass() == JavaLangObject);
  EXPECT_TRUE(MyClass->HasSuperClass());
  EXPECT_EQ(class_loader.get(), MyClass->GetClassLoader());
  EXPECT_EQ(Class::kStatusResolved, MyClass->GetStatus());
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
  EXPECT_EQ(0U, kh.NumInterfaces());

  EXPECT_EQ(JavaLangObject->GetClass()->GetClass(), MyClass->GetClass()->GetClass());

  // created by class_linker
  AssertArrayClass("[C", "C", NULL);
  AssertArrayClass("[Ljava/lang/Object;", "Ljava/lang/Object;", NULL);
  // synthesized on the fly
  AssertArrayClass("[[C", "[C", NULL);
  AssertArrayClass("[[[LMyClass;", "[[LMyClass;", class_loader.get());
  // or not available at all
  AssertNonExistentClass("[[[[LNonExistentClass;");
}

TEST_F(ClassLinkerTest, LibCore) {
  AssertDexFile(java_lang_dex_file_.get(), NULL);
}

// The first reference array element must be a multiple of 8 bytes from the
// start of the object
TEST_F(ClassLinkerTest, ValidateObjectArrayElementsOffset) {
  Class* array_class = class_linker_->FindSystemClass("[Ljava/lang/String;");
  ObjectArray<String>* array = ObjectArray<String>::Alloc(array_class, 0);
  uint32_t array_offset = reinterpret_cast<uint32_t>(array);
  uint32_t data_offset =
      array_offset + ObjectArray<String>::DataOffset().Uint32Value();
  EXPECT_EQ(16U, data_offset - array_offset);
}

TEST_F(ClassLinkerTest, ValidatePrimitiveArrayElementsOffset) {
  SirtRef<LongArray> array(LongArray::Alloc(0));
  EXPECT_EQ(class_linker_->FindSystemClass("[J"), array->GetClass());
  uint32_t array_offset = reinterpret_cast<uint32_t>(array.get());
  uint32_t data_offset = reinterpret_cast<uint32_t>(array->GetData());
  EXPECT_EQ(16U, data_offset - array_offset);
}

TEST_F(ClassLinkerTest, ValidateBoxedTypes) {
  // Validate that the "value" field is always the 0th field in each of java.lang's box classes.
  // This lets UnboxPrimitive avoid searching for the field by name at runtime.
  Class* c;
  c = class_linker_->FindClass("Ljava/lang/Boolean;", NULL);
  FieldHelper fh(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass("Ljava/lang/Byte;", NULL);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass("Ljava/lang/Character;", NULL);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass("Ljava/lang/Double;", NULL);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass("Ljava/lang/Float;", NULL);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass("Ljava/lang/Integer;", NULL);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass("Ljava/lang/Long;", NULL);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
  c = class_linker_->FindClass("Ljava/lang/Short;", NULL);
  fh.ChangeField(c->GetIFields()->Get(0));
  EXPECT_STREQ("value", fh.GetName());
}

TEST_F(ClassLinkerTest, TwoClassLoadersOneClass) {
  SirtRef<ClassLoader> class_loader_1(LoadDex("MyClass"));
  SirtRef<ClassLoader> class_loader_2(LoadDex("MyClass"));
  Class* MyClass_1 = class_linker_->FindClass("LMyClass;", class_loader_1.get());
  Class* MyClass_2 = class_linker_->FindClass("LMyClass;", class_loader_2.get());
  EXPECT_TRUE(MyClass_1 != NULL);
  EXPECT_TRUE(MyClass_2 != NULL);
  EXPECT_NE(MyClass_1, MyClass_2);
}

TEST_F(ClassLinkerTest, StaticFields) {
  SirtRef<ClassLoader> class_loader(LoadDex("Statics"));
  Class* statics = class_linker_->FindClass("LStatics;", class_loader.get());
  class_linker_->EnsureInitialized(statics, true);

  // Static final primitives that are initialized by a compile-time constant
  // expression resolve to a copy of a constant value from the constant pool.
  // So <clinit> should be null.
  Method* clinit = statics->FindDirectMethod("<clinit>", "()V");
  EXPECT_TRUE(clinit == NULL);

  EXPECT_EQ(9U, statics->NumStaticFields());

  Field* s0 = statics->FindStaticField("s0", "Z");
  FieldHelper fh(s0);
  EXPECT_STREQ(ClassHelper(s0->GetClass()).GetDescriptor(), "Ljava/lang/reflect/Field;");
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimBoolean);
  EXPECT_EQ(true, s0->GetBoolean(NULL));
  s0->SetBoolean(NULL, false);

  Field* s1 = statics->FindStaticField("s1", "B");
  fh.ChangeField(s1);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimByte);
  EXPECT_EQ(5, s1->GetByte(NULL));
  s1->SetByte(NULL, 6);

  Field* s2 = statics->FindStaticField("s2", "C");
  fh.ChangeField(s2);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimChar);
  EXPECT_EQ('a', s2->GetChar(NULL));
  s2->SetChar(NULL, 'b');

  Field* s3 = statics->FindStaticField("s3", "S");
  fh.ChangeField(s3);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimShort);
  EXPECT_EQ(-536, s3->GetShort(NULL));
  s3->SetShort(NULL, -535);

  Field* s4 = statics->FindStaticField("s4", "I");
  fh.ChangeField(s4);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimInt);
  EXPECT_EQ(2000000000, s4->GetInt(NULL));
  s4->SetInt(NULL, 2000000001);

  Field* s5 = statics->FindStaticField("s5", "J");
  fh.ChangeField(s5);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimLong);
  EXPECT_EQ(0x1234567890abcdefLL, s5->GetLong(NULL));
  s5->SetLong(NULL, 0x34567890abcdef12LL);

  Field* s6 = statics->FindStaticField("s6", "F");
  fh.ChangeField(s6);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimFloat);
  EXPECT_EQ(0.5, s6->GetFloat(NULL));
  s6->SetFloat(NULL, 0.75);

  Field* s7 = statics->FindStaticField("s7", "D");
  fh.ChangeField(s7);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimDouble);
  EXPECT_EQ(16777217, s7->GetDouble(NULL));
  s7->SetDouble(NULL, 16777219);

  Field* s8 = statics->FindStaticField("s8", "Ljava/lang/String;");
  fh.ChangeField(s8);
  EXPECT_TRUE(fh.GetTypeAsPrimitiveType() == Primitive::kPrimNot);
  EXPECT_TRUE(s8->GetObject(NULL)->AsString()->Equals("android"));
  s8->SetObject(NULL, String::AllocFromModifiedUtf8("robot"));

  // TODO: Remove EXPECT_FALSE when GCC can handle EXPECT_EQ
  // http://code.google.com/p/googletest/issues/detail?id=322
  EXPECT_FALSE(                   s0->GetBoolean(NULL));
  EXPECT_EQ(6,                    s1->GetByte(NULL));
  EXPECT_EQ('b',                  s2->GetChar(NULL));
  EXPECT_EQ(-535,                 s3->GetShort(NULL));
  EXPECT_EQ(2000000001,           s4->GetInt(NULL));
  EXPECT_EQ(0x34567890abcdef12LL, s5->GetLong(NULL));
  EXPECT_EQ(0.75,                 s6->GetFloat(NULL));
  EXPECT_EQ(16777219,             s7->GetDouble(NULL));
  EXPECT_TRUE(s8->GetObject(NULL)->AsString()->Equals("robot"));
}

TEST_F(ClassLinkerTest, Interfaces) {
  SirtRef<ClassLoader> class_loader(LoadDex("Interfaces"));
  Class* I = class_linker_->FindClass("LInterfaces$I;", class_loader.get());
  Class* J = class_linker_->FindClass("LInterfaces$J;", class_loader.get());
  Class* K = class_linker_->FindClass("LInterfaces$K;", class_loader.get());
  Class* A = class_linker_->FindClass("LInterfaces$A;", class_loader.get());
  Class* B = class_linker_->FindClass("LInterfaces$B;", class_loader.get());
  EXPECT_TRUE(I->IsAssignableFrom(A));
  EXPECT_TRUE(J->IsAssignableFrom(A));
  EXPECT_TRUE(J->IsAssignableFrom(K));
  EXPECT_TRUE(K->IsAssignableFrom(B));
  EXPECT_TRUE(J->IsAssignableFrom(B));

  Method* Ii = I->FindVirtualMethod("i", "()V");
  Method* Jj1 = J->FindVirtualMethod("j1", "()V");
  Method* Jj2 = J->FindVirtualMethod("j2", "()V");
  Method* Kj1 = K->FindInterfaceMethod("j1", "()V");
  Method* Kj2 = K->FindInterfaceMethod("j2", "()V");
  Method* Kk = K->FindInterfaceMethod("k", "()V");
  Method* Ai = A->FindVirtualMethod("i", "()V");
  Method* Aj1 = A->FindVirtualMethod("j1", "()V");
  Method* Aj2 = A->FindVirtualMethod("j2", "()V");
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
  EXPECT_EQ(Ai, A->FindVirtualMethodForInterface(Ii, true));
  EXPECT_EQ(Aj1, A->FindVirtualMethodForInterface(Jj1, true));
  EXPECT_EQ(Aj2, A->FindVirtualMethodForInterface(Jj2, true));
  EXPECT_EQ(Ai, A->FindVirtualMethodForVirtualOrInterface(Ii));
  EXPECT_EQ(Aj1, A->FindVirtualMethodForVirtualOrInterface(Jj1));
  EXPECT_EQ(Aj2, A->FindVirtualMethodForVirtualOrInterface(Jj2));
}

TEST_F(ClassLinkerTest, ResolveVerifyAndClinit) {
  // pretend we are trying to get the static storage for the StaticsFromCode class.

  // case 1, get the uninitialized storage from StaticsFromCode.<clinit>
  // case 2, get the initialized storage from StaticsFromCode.getS0

  SirtRef<ClassLoader> class_loader(LoadDex("StaticsFromCode"));
  const DexFile* dex_file = ClassLoader::GetCompileTimeClassPath(class_loader.get())[0];
  CHECK(dex_file != NULL);

  Class* klass = class_linker_->FindClass("LStaticsFromCode;", class_loader.get());
  Method* clinit = klass->FindDirectMethod("<clinit>", "()V");
  Method* getS0 = klass->FindDirectMethod("getS0", "()Ljava/lang/Object;");
  const DexFile::StringId* string_id = dex_file->FindStringId("LStaticsFromCode;");
  ASSERT_TRUE(string_id != NULL);
  const DexFile::TypeId* type_id = dex_file->FindTypeId(dex_file->GetIndexForStringId(*string_id));
  ASSERT_TRUE(type_id != NULL);
  uint32_t type_idx = dex_file->GetIndexForTypeId(*type_id);
  EXPECT_TRUE(clinit->GetDexCacheInitializedStaticStorage()->Get(type_idx) == NULL);
  StaticStorageBase* uninit = ResolveVerifyAndClinit(type_idx, clinit, Thread::Current(), true, false);
  EXPECT_TRUE(uninit != NULL);
  EXPECT_TRUE(clinit->GetDexCacheInitializedStaticStorage()->Get(type_idx) == NULL);
  StaticStorageBase* init = ResolveVerifyAndClinit(type_idx, getS0, Thread::Current(), true, false);
  EXPECT_TRUE(init != NULL);
  EXPECT_EQ(init, clinit->GetDexCacheInitializedStaticStorage()->Get(type_idx));
}

TEST_F(ClassLinkerTest, FinalizableBit) {
  Class* c;

  // Object has a finalize method, but we know it's empty.
  c = class_linker_->FindSystemClass("Ljava/lang/Object;");
  EXPECT_FALSE(c->IsFinalizable());

  // Enum has a finalize method to prevent its subclasses from implementing one.
  c = class_linker_->FindSystemClass("Ljava/lang/Enum;");
  EXPECT_FALSE(c->IsFinalizable());

  // RoundingMode is an enum.
  c = class_linker_->FindSystemClass("Ljava/math/RoundingMode;");
  EXPECT_FALSE(c->IsFinalizable());

  // RandomAccessFile extends Object and overrides finalize.
  c = class_linker_->FindSystemClass("Ljava/io/RandomAccessFile;");
  EXPECT_TRUE(c->IsFinalizable());

  // FileInputStream is finalizable and extends InputStream which isn't.
  c = class_linker_->FindSystemClass("Ljava/io/InputStream;");
  EXPECT_FALSE(c->IsFinalizable());
  c = class_linker_->FindSystemClass("Ljava/io/FileInputStream;");
  EXPECT_TRUE(c->IsFinalizable());

  // ScheduledThreadPoolExecutor doesn't have a finalize method but
  // extends ThreadPoolExecutor which does.
  c = class_linker_->FindSystemClass("Ljava/util/concurrent/ThreadPoolExecutor;");
  EXPECT_TRUE(c->IsFinalizable());
  c = class_linker_->FindSystemClass("Ljava/util/concurrent/ScheduledThreadPoolExecutor;");
  EXPECT_TRUE(c->IsFinalizable());
}

TEST_F(ClassLinkerTest, ClassRootDescriptors) {
  ClassHelper kh;
  for (int i = 0; i < ClassLinker::kClassRootsMax; i++) {
    Class* klass = class_linker_->GetClassRoot(ClassLinker::ClassRoot(i));
    kh.ChangeClass(klass);
    EXPECT_TRUE(kh.GetDescriptor() != NULL);
    EXPECT_STREQ(kh.GetDescriptor(),
                 class_linker_->GetClassRootDescriptor(ClassLinker::ClassRoot(i))) << " i = " << i;
  }
}

}  // namespace art
