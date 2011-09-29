// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_linker.h"

#include <string>

#include "UniquePtr.h"
#include "common_test.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "heap.h"

namespace art {

class ClassLinkerTest : public CommonTest {
 protected:
  void AssertNonExistentClass(const StringPiece& descriptor) {
    EXPECT_TRUE(class_linker_->FindSystemClass(descriptor) == NULL);
    Thread* self = Thread::Current();
    EXPECT_TRUE(self->IsExceptionPending());
    Object* exception = self->GetException();
    self->ClearException();
    Class* exception_class = class_linker_->FindSystemClass("Ljava/lang/NoClassDefFoundError;");
    EXPECT_TRUE(exception->InstanceOf(exception_class));
  }

  void AssertPrimitiveClass(const StringPiece& descriptor) {
    AssertPrimitiveClass(descriptor, class_linker_->FindSystemClass(descriptor));
  }

  void AssertPrimitiveClass(const StringPiece& descriptor, const Class* primitive) {
    ASSERT_TRUE(primitive != NULL);
    ASSERT_TRUE(primitive->GetClass() != NULL);
    ASSERT_EQ(primitive->GetClass(), primitive->GetClass()->GetClass());
    EXPECT_TRUE(primitive->GetClass()->GetSuperClass() != NULL);
    ASSERT_TRUE(primitive->GetDescriptor()->Equals(descriptor));
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
    EXPECT_EQ(0U, primitive->NumInterfaces());
    EXPECT_TRUE(primitive->GetVTable() == NULL);
    EXPECT_EQ(0, primitive->GetIfTableCount());
    EXPECT_TRUE(primitive->GetIfTable() == NULL);
  }

  void AssertArrayClass(const StringPiece& array_descriptor,
                        const StringPiece& component_type,
                        const ClassLoader* class_loader) {
    Class* array = class_linker_->FindClass(array_descriptor, class_loader);
    EXPECT_TRUE(array->GetComponentType()->GetDescriptor()->Equals(component_type));
    EXPECT_EQ(class_loader, array->GetClassLoader());
    AssertArrayClass(array_descriptor, array);
  }

  void AssertArrayClass(const StringPiece& array_descriptor, Class* array) {
    ASSERT_TRUE(array != NULL);
    ASSERT_TRUE(array->GetClass() != NULL);
    ASSERT_EQ(array->GetClass(), array->GetClass()->GetClass());
    EXPECT_TRUE(array->GetClass()->GetSuperClass() != NULL);
    ASSERT_TRUE(array->GetDescriptor()->Equals(array_descriptor));
    EXPECT_TRUE(array->GetSuperClass() != NULL);
    EXPECT_EQ(class_linker_->FindSystemClass("Ljava/lang/Object;"), array->GetSuperClass());
    EXPECT_TRUE(array->HasSuperClass());
    ASSERT_TRUE(array->GetComponentType() != NULL);
    ASSERT_TRUE(array->GetComponentType()->GetDescriptor() != NULL);
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
    EXPECT_EQ(2U, array->NumInterfaces());
    EXPECT_TRUE(array->GetVTable() != NULL);
    EXPECT_EQ(2, array->GetIfTableCount());
    ObjectArray<InterfaceEntry>* iftable = array->GetIfTable();
    ASSERT_TRUE(iftable != NULL);
    EXPECT_TRUE(iftable->Get(0)->GetInterface()->GetDescriptor()->Equals("Ljava/lang/Cloneable;"));
    EXPECT_TRUE(iftable->Get(1)->GetInterface()->GetDescriptor()->Equals("Ljava/io/Serializable;"));
  }

  void AssertMethod(Class* klass, Method* method) {
    EXPECT_TRUE(method != NULL);
    EXPECT_TRUE(method->GetClass() != NULL);
    EXPECT_TRUE(method->GetName() != NULL);
    EXPECT_TRUE(method->GetSignature() != NULL);

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
    EXPECT_TRUE(field != NULL);
    EXPECT_TRUE(field->GetClass() != NULL);
    EXPECT_EQ(klass, field->GetDeclaringClass());
    EXPECT_TRUE(field->GetName() != NULL);
    EXPECT_TRUE(field->GetType() != NULL);
  }

  void AssertClass(const StringPiece& descriptor, Class* klass) {
    EXPECT_TRUE(klass->GetDescriptor()->Equals(descriptor));
    if (klass->GetDescriptor()->Equals(String::AllocFromModifiedUtf8("Ljava/lang/Object;"))) {
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
    EXPECT_TRUE(Class::IsInSamePackage(klass->GetDescriptor(), klass->GetDescriptor()));
    if (klass->IsInterface()) {
      EXPECT_TRUE(klass->IsAbstract());
      if (klass->NumDirectMethods() == 1) {
        EXPECT_TRUE(klass->GetDirectMethod(0)->GetName()->Equals("<clinit>"));
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
    for (size_t i = 0; i < klass->NumReferenceInstanceFields(); i++) {
      Field* field = klass->GetInstanceField(i);
      Class* field_type = field->GetType();
      ASSERT_TRUE(field_type != NULL);
      ASSERT_TRUE(!field_type->IsPrimitive());
    }
    for (size_t i = klass->NumReferenceInstanceFields(); i < klass->NumInstanceFields(); i++) {
      Field* field = klass->GetInstanceField(i);
      Class* field_type = field->GetType();
      ASSERT_TRUE(field_type != NULL);
      EXPECT_TRUE(field_type->IsPrimitive());
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

  void AssertDexFileClass(ClassLoader* class_loader, const StringPiece& descriptor) {
    ASSERT_TRUE(descriptor != NULL);
    Class* klass = class_linker_->FindSystemClass(descriptor);
    ASSERT_TRUE(klass != NULL);
    EXPECT_TRUE(klass->GetDescriptor()->Equals(descriptor));
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
  bool instance;
  std::string class_descriptor;
  std::vector<CheckOffset> offsets;

  bool Check() {
    Class* klass = Runtime::Current()->GetClassLinker()->FindSystemClass(class_descriptor);
    CHECK(klass != NULL) << class_descriptor;

    bool error = false;

    if (!klass->IsClassClass() && instance) {
      size_t expected_size = instance ? klass->GetObjectSize() : klass->GetClassSize();
      if (sizeof(T) != expected_size) {
        LG << "Class size mismatch:"
           << " class=" << class_descriptor
           << " Java=" << expected_size
           << " C++=" << sizeof(T);
        error = true;
      }
    }

    size_t num_fields = instance ? klass->NumInstanceFields() : klass->NumStaticFields();
    if (offsets.size() != num_fields) {
      LG << "Field count mismatch:"
         << " class=" << class_descriptor
         << " Java=" << num_fields
         << " C++=" << offsets.size();
      error = true;
    }

    for (size_t i = 0; i < offsets.size(); i++) {
      Field* field = instance ? klass->GetInstanceField(i) : klass->GetStaticField(i);
      if (!field->GetName()->Equals(offsets[i].java_name)) {
        error = true;
      }
    }
    if (error) {
      for (size_t i = 0; i < offsets.size(); i++) {
        CheckOffset& offset = offsets[i];
        Field* field = instance ? klass->GetInstanceField(i) : klass->GetStaticField(i);
        if (!field->GetName()->Equals(offsets[i].java_name)) {
          LG << "JAVA FIELD ORDER MISMATCH NEXT LINE:";
        }
        LG << "Java field order:"
           << " i=" << i << " class=" << class_descriptor
           << " Java=" << field->GetName()->ToModifiedUtf8()
           << " CheckOffsets=" << offset.java_name;
      }
    }

    for (size_t i = 0; i < offsets.size(); i++) {
      CheckOffset& offset = offsets[i];
      Field* field = instance ? klass->GetInstanceField(i) : klass->GetStaticField(i);
      if (field->GetOffset().Uint32Value() != offset.cpp_offset) {
        error = true;
      }
    }
    if (error) {
      for (size_t i = 0; i < offsets.size(); i++) {
        CheckOffset& offset = offsets[i];
        Field* field = instance ? klass->GetInstanceField(i) : klass->GetStaticField(i);
        if (field->GetOffset().Uint32Value() != offset.cpp_offset) {
          LG << "OFFSET MISMATCH NEXT LINE:";
        }
        LG << "Offset: class=" << class_descriptor << " field=" << offset.java_name
           << " Java=" << field->GetOffset().Uint32Value() << " C++=" << offset.cpp_offset;
      }
    }

    return !error;
  };
};

// Note that ClassLinkerTest.ValidateFieldOrderOfJavaCppUnionClasses
// is first since if it is failing, others are unlikely to succeed.

struct ObjectOffsets : public CheckOffsets<Object> {
  ObjectOffsets() {
    instance = true;
    class_descriptor = "Ljava/lang/Object;";

    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Object, klass_),   "shadow$_klass_"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Object, monitor_), "shadow$_monitor_"));
  };
};

struct AccessibleObjectOffsets : public CheckOffsets<AccessibleObject> {
  AccessibleObjectOffsets() {
    instance = true;
    class_descriptor = "Ljava/lang/reflect/AccessibleObject;";
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(AccessibleObject, java_flag_), "flag"));
  };
};

struct FieldOffsets : public CheckOffsets<Field> {
  FieldOffsets() {
    instance = true;
    class_descriptor = "Ljava/lang/reflect/Field;";

    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, declaring_class_),               "declaringClass"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, generic_type_),                  "genericType"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, name_),                          "name"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, type_),                          "type"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, generic_types_are_initialized_), "genericTypesAreInitialized"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, access_flags_),                  "shadow$_access_flags_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, offset_),                        "shadow$_offset_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, type_idx_),                      "shadow$_type_idx_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Field, slot_),                          "slot"));
  };
};

struct MethodOffsets : public CheckOffsets<Method> {
  MethodOffsets() {
    instance = true;
    class_descriptor = "Ljava/lang/reflect/Method;";

    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, declaring_class_),                      "declaringClass"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, java_exception_types_),                 "exceptionTypes"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, java_formal_type_parameters_),          "formalTypeParameters"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, java_generic_exception_types_),         "genericExceptionTypes"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, java_generic_parameter_types_),         "genericParameterTypes"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, java_generic_return_type_),             "genericReturnType"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, name_),                                 "name"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, java_parameter_types_),                 "parameterTypes"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, java_return_type_),                     "returnType"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, code_array_),                           "shadow$_code_array_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_code_and_direct_methods_),    "shadow$_dex_cache_code_and_direct_methods_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_initialized_static_storage_), "shadow$_dex_cache_initialized_static_storage_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_resolved_fields_),            "shadow$_dex_cache_resolved_fields_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_resolved_methods_),           "shadow$_dex_cache_resolved_methods_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_resolved_types_),             "shadow$_dex_cache_resolved_types_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, dex_cache_strings_),                    "shadow$_dex_cache_strings_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, invoke_stub_array_),                    "shadow$_invoke_stub_array_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, mapping_table_),                        "shadow$_mapping_table_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, register_map_data_),                    "shadow$_register_map_data_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, register_map_header_),                  "shadow$_register_map_header_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, shorty_),                               "shadow$_shorty_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, signature_),                            "shadow$_signature_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, vmap_table_),                           "shadow$_vmap_table_"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, java_generic_types_are_initialized_),   "genericTypesAreInitialized"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, access_flags_),                         "shadow$_access_flags_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, code_),                                 "shadow$_code_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, code_item_offset_),                     "shadow$_code_item_offset_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, core_spill_mask_),                      "shadow$_core_spill_mask_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, fp_spill_mask_),                        "shadow$_fp_spill_mask_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, frame_size_in_bytes_),                  "shadow$_frame_size_in_bytes_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, invoke_stub_),                          "shadow$_invoke_stub_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, java_return_type_idx_),                 "shadow$_java_return_type_idx_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, method_index_),                         "shadow$_method_index_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, native_method_),                        "shadow$_native_method_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, num_ins_),                              "shadow$_num_ins_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, num_outs_),                             "shadow$_num_outs_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, num_registers_),                        "shadow$_num_registers_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, proto_idx_),                            "shadow$_proto_idx_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, return_pc_offset_in_bytes_),            "shadow$_return_pc_offset_in_bytes_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Method, java_slot_),                            "slot"));
  };
};

struct ConstructorOffsets : public MethodOffsets {
  ConstructorOffsets() : MethodOffsets() {
    // We use Method* for both java.lang.reflect.Constructor and java.lang.reflect.Method.
    class_descriptor = "Ljava/lang/reflect/Constructor;";
  }
};

struct ClassOffsets : public CheckOffsets<Class> {
  ClassOffsets() {
    instance = true;
    class_descriptor = "Ljava/lang/Class;";

    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, name_),                          "name"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, class_loader_),                  "shadow$_class_loader_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, component_type_),                "shadow$_component_type_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, descriptor_),                    "shadow$_descriptor_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, dex_cache_),                     "shadow$_dex_cache_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, direct_methods_),                "shadow$_direct_methods_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, ifields_),                       "shadow$_ifields_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, iftable_),                       "shadow$_iftable_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, interfaces_),                    "shadow$_interfaces_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, interfaces_type_idx_),           "shadow$_interfaces_type_idx_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, sfields_),                       "shadow$_sfields_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, source_file_),                   "shadow$_source_file_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, super_class_),                   "shadow$_super_class_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, verify_error_class_),            "shadow$_verify_error_class_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, virtual_methods_),               "shadow$_virtual_methods_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, vtable_),                        "shadow$_vtable_"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, access_flags_),                  "shadow$_access_flags_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, class_size_),                    "shadow$_class_size_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, clinit_thread_id_),              "shadow$_clinit_thread_id_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, num_reference_instance_fields_), "shadow$_num_reference_instance_fields_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, num_reference_static_fields_),   "shadow$_num_reference_static_fields_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, object_size_),                   "shadow$_object_size_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, primitive_type_),                "shadow$_primitive_type_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, reference_instance_offsets_),    "shadow$_reference_instance_offsets_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, reference_static_offsets_),      "shadow$_reference_static_offsets_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, status_),                        "shadow$_status_"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Class, super_class_type_idx_),          "shadow$_super_class_type_idx_"));
  };
};

struct StringOffsets : public CheckOffsets<String> {
  StringOffsets() {
    instance = true;
    class_descriptor = "Ljava/lang/String;";

    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(String, array_),     "value"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(String, count_),     "count"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(String, hash_code_), "hashCode"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(String, offset_),    "offset"));
  };
};

struct ThrowableOffsets : public CheckOffsets<Throwable> {
  ThrowableOffsets() {
    instance = true;
    class_descriptor = "Ljava/lang/Throwable;";

    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Throwable, cause_),                 "cause"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Throwable, detail_message_),        "detailMessage"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Throwable, stack_state_),           "stackState"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Throwable, stack_trace_),           "stackTrace"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(Throwable, suppressed_exceptions_), "suppressedExceptions"));
  };
};

struct StackTraceElementOffsets : public CheckOffsets<StackTraceElement> {
  StackTraceElementOffsets() {
    instance = true;
    class_descriptor = "Ljava/lang/StackTraceElement;";

    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StackTraceElement, declaring_class_), "declaringClass"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StackTraceElement, file_name_),       "fileName"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StackTraceElement, method_name_),     "methodName"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(StackTraceElement, line_number_),     "lineNumber"));
  };
};

struct ClassLoaderOffsets : public CheckOffsets<ClassLoader> {
  ClassLoaderOffsets() {
    instance = true;
    class_descriptor = "Ljava/lang/ClassLoader;";

    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(ClassLoader, packages_), "packages"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(ClassLoader, parent_),   "parent"));
  };
};

struct BaseDexClassLoaderOffsets : public CheckOffsets<BaseDexClassLoader> {
  BaseDexClassLoaderOffsets() {
    instance = true;
    class_descriptor = "Ldalvik/system/BaseDexClassLoader;";

    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(BaseDexClassLoader, original_path_), "originalPath"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(BaseDexClassLoader, path_list_),     "pathList"));
  };
};

struct PathClassLoaderOffsets : public CheckOffsets<PathClassLoader> {
  PathClassLoaderOffsets() {
    instance = true;
    class_descriptor = "Ldalvik/system/PathClassLoader;";
  };
};

struct ClassClassOffsets : public CheckOffsets<ClassClass> {
  ClassClassOffsets() {
    instance = false;
    class_descriptor = "Ljava/lang/Class;";

    // padding 32-bit
    CHECK_EQ(OFFSETOF_MEMBER(ClassClass, padding_) + 4,
             OFFSETOF_MEMBER(ClassClass, serialVersionUID_));

    // alphabetical 64-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(ClassClass, serialVersionUID_), "serialVersionUID"));
  };
};

struct StringClassOffsets : public CheckOffsets<StringClass> {
  StringClassOffsets() {
    instance = false;
    class_descriptor = "Ljava/lang/String;";

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
  FieldClassOffsets() {
    instance = false;
    class_descriptor = "Ljava/lang/reflect/Field;";

    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(FieldClass, ORDER_BY_NAME_AND_DECLARING_CLASS_), "ORDER_BY_NAME_AND_DECLARING_CLASS"));

    // alphabetical 32-bit
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(FieldClass, TYPE_BOOLEAN_), "TYPE_BOOLEAN"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(FieldClass, TYPE_BYTE_),    "TYPE_BYTE"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(FieldClass, TYPE_CHAR_),    "TYPE_CHAR"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(FieldClass, TYPE_DOUBLE_),  "TYPE_DOUBLE"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(FieldClass, TYPE_FLOAT_),   "TYPE_FLOAT"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(FieldClass, TYPE_INTEGER_), "TYPE_INTEGER"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(FieldClass, TYPE_LONG_),    "TYPE_LONG"));
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(FieldClass, TYPE_SHORT_),   "TYPE_SHORT"));
  };
};

struct MethodClassOffsets : public CheckOffsets<MethodClass> {
  MethodClassOffsets() {
    instance = false;
    class_descriptor = "Ljava/lang/reflect/Method;";

    // alphabetical references
    offsets.push_back(CheckOffset(OFFSETOF_MEMBER(MethodClass, ORDER_BY_SIGNATURE_), "ORDER_BY_SIGNATURE"));
  };
};

// C++ fields must exactly match the fields in the Java classes. If this fails,
// reorder the fields in the C++ class. Managed class fields are ordered by
// ClassLinker::LinkFields.
TEST_F(ClassLinkerTest, ValidateFieldOrderOfJavaCppUnionClasses) {
  EXPECT_TRUE(ObjectOffsets().Check());
  EXPECT_TRUE(AccessibleObjectOffsets().Check());
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
  const ClassLoader* class_loader = LoadDex("Nested");

  Class* outer = class_linker_->FindClass("LNested;", class_loader);
  ASSERT_TRUE(outer != NULL);
  EXPECT_EQ(0U, outer->NumVirtualMethods());
  EXPECT_EQ(1U, outer->NumDirectMethods());

  Class* inner = class_linker_->FindClass("LNested$Inner;", class_loader);
  ASSERT_TRUE(inner != NULL);
  EXPECT_EQ(0U, inner->NumVirtualMethods());
  EXPECT_EQ(1U, inner->NumDirectMethods());
}

TEST_F(ClassLinkerTest, FindClass_Primitives) {
  StringPiece expected = "BCDFIJSZV";
  for (int ch = 0; ch < 255; ch++) {
    char* s = reinterpret_cast<char*>(&ch);
    StringPiece descriptor(s, 1);
    if (expected.find(ch) == StringPiece::npos) {
      AssertNonExistentClass(descriptor);
    } else {
      AssertPrimitiveClass(descriptor);
    }
  }
}

TEST_F(ClassLinkerTest, FindClass) {
  Class* JavaLangObject = class_linker_->FindSystemClass("Ljava/lang/Object;");
  ASSERT_TRUE(JavaLangObject != NULL);
  ASSERT_TRUE(JavaLangObject->GetClass() != NULL);
  ASSERT_EQ(JavaLangObject->GetClass(), JavaLangObject->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, JavaLangObject->GetClass()->GetSuperClass());
  ASSERT_TRUE(JavaLangObject->GetDescriptor()->Equals("Ljava/lang/Object;"));
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
  EXPECT_TRUE(JavaLangObject->GetInstanceField(0)->GetName()->Equals("shadow$_klass_"));
  EXPECT_TRUE(JavaLangObject->GetInstanceField(1)->GetName()->Equals("shadow$_monitor_"));

  EXPECT_EQ(0U, JavaLangObject->NumStaticFields());
  EXPECT_EQ(0U, JavaLangObject->NumInterfaces());

  const ClassLoader* class_loader = LoadDex("MyClass");
  AssertNonExistentClass("LMyClass;");
  Class* MyClass = class_linker_->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(MyClass != NULL);
  ASSERT_TRUE(MyClass->GetClass() != NULL);
  ASSERT_EQ(MyClass->GetClass(), MyClass->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, MyClass->GetClass()->GetSuperClass());
  ASSERT_TRUE(MyClass->GetDescriptor()->Equals("LMyClass;"));
  EXPECT_TRUE(MyClass->GetSuperClass() == JavaLangObject);
  EXPECT_TRUE(MyClass->HasSuperClass());
  EXPECT_EQ(class_loader, MyClass->GetClassLoader());
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
  EXPECT_EQ(0U, MyClass->NumInterfaces());

  EXPECT_EQ(JavaLangObject->GetClass()->GetClass(), MyClass->GetClass()->GetClass());

  // created by class_linker
  AssertArrayClass("[C", "C", NULL);
  AssertArrayClass("[Ljava/lang/Object;", "Ljava/lang/Object;", NULL);
  // synthesized on the fly
  AssertArrayClass("[[C", "[C", NULL);
  AssertArrayClass("[[[LMyClass;", "[[LMyClass;", class_loader);
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
  LongArray* array = LongArray::Alloc(0);
  EXPECT_EQ(class_linker_->FindSystemClass("[J"), array->GetClass());
  uint32_t array_offset = reinterpret_cast<uint32_t>(array);
  uint32_t data_offset = reinterpret_cast<uint32_t>(array->GetData());
  EXPECT_EQ(16U, data_offset - array_offset);
}

TEST_F(ClassLinkerTest, ValidateBoxedTypes) {
  // Validate that the "value" field is always the 0th field in each of java.lang's box classes.
  // This lets UnboxPrimitive avoid searching for the field by name at runtime.
  Class* c;
  c = class_linker_->FindClass("Ljava/lang/Boolean;", NULL);
  EXPECT_EQ("value", c->GetIFields()->Get(0)->GetName()->ToModifiedUtf8());
  c = class_linker_->FindClass("Ljava/lang/Byte;", NULL);
  EXPECT_EQ("value", c->GetIFields()->Get(0)->GetName()->ToModifiedUtf8());
  c = class_linker_->FindClass("Ljava/lang/Character;", NULL);
  EXPECT_EQ("value", c->GetIFields()->Get(0)->GetName()->ToModifiedUtf8());
  c = class_linker_->FindClass("Ljava/lang/Double;", NULL);
  EXPECT_EQ("value", c->GetIFields()->Get(0)->GetName()->ToModifiedUtf8());
  c = class_linker_->FindClass("Ljava/lang/Float;", NULL);
  EXPECT_EQ("value", c->GetIFields()->Get(0)->GetName()->ToModifiedUtf8());
  c = class_linker_->FindClass("Ljava/lang/Integer;", NULL);
  EXPECT_EQ("value", c->GetIFields()->Get(0)->GetName()->ToModifiedUtf8());
  c = class_linker_->FindClass("Ljava/lang/Long;", NULL);
  EXPECT_EQ("value", c->GetIFields()->Get(0)->GetName()->ToModifiedUtf8());
  c = class_linker_->FindClass("Ljava/lang/Short;", NULL);
  EXPECT_EQ("value", c->GetIFields()->Get(0)->GetName()->ToModifiedUtf8());
}

TEST_F(ClassLinkerTest, TwoClassLoadersOneClass) {
  const ClassLoader* class_loader_1 = LoadDex("MyClass");
  const ClassLoader* class_loader_2 = LoadDex("MyClass");
  Class* MyClass_1 = class_linker_->FindClass("LMyClass;", class_loader_1);
  Class* MyClass_2 = class_linker_->FindClass("LMyClass;", class_loader_2);
  EXPECT_TRUE(MyClass_1 != NULL);
  EXPECT_TRUE(MyClass_2 != NULL);
  EXPECT_NE(MyClass_1, MyClass_2);
}

TEST_F(ClassLinkerTest, StaticFields) {
  // TODO: uncomment expectations of initial values when InitializeClass works
  const ClassLoader* class_loader = LoadDex("Statics");
  Class* statics = class_linker_->FindClass("LStatics;", class_loader);
  class_linker_->EnsureInitialized(statics, true);

  EXPECT_EQ(10U, statics->NumStaticFields());

  Field* s0 = statics->FindStaticField("s0", class_linker_->FindClass("Z", class_loader));
  EXPECT_TRUE(s0->GetClass()->GetDescriptor()->Equals("Ljava/lang/reflect/Field;"));
  EXPECT_TRUE(s0->GetType()->IsPrimitiveBoolean());
  // EXPECT_EQ(true, s0->GetBoolean(NULL)); // TODO: needs clinit to be run?
  s0->SetBoolean(NULL, false);

  Field* s1 = statics->FindStaticField("s1", class_linker_->FindClass("B", class_loader));
  EXPECT_TRUE(s1->GetType()->IsPrimitiveByte());
  // EXPECT_EQ(5, s1->GetByte(NULL));  // TODO: needs clinit to be run?
  s1->SetByte(NULL, 6);

  Field* s2 = statics->FindStaticField("s2", class_linker_->FindClass("C", class_loader));
  EXPECT_TRUE(s2->GetType()->IsPrimitiveChar());
  // EXPECT_EQ('a', s2->GetChar(NULL));  // TODO: needs clinit to be run?
  s2->SetChar(NULL, 'b');

  Field* s3 = statics->FindStaticField("s3", class_linker_->FindClass("S", class_loader));
  EXPECT_TRUE(s3->GetType()->IsPrimitiveShort());
  // EXPECT_EQ(65000, s3->GetShort(NULL));  // TODO: needs clinit to be run?
  s3->SetShort(NULL, 65001);

  Field* s4 = statics->FindStaticField("s4", class_linker_->FindClass("I", class_loader));
  EXPECT_TRUE(s4->GetType()->IsPrimitiveInt());
  // EXPECT_EQ(2000000000, s4->GetInt(NULL));  // TODO: needs clinit to be run?
  s4->SetInt(NULL, 2000000001);

  Field* s5 = statics->FindStaticField("s5", class_linker_->FindClass("J", class_loader));
  EXPECT_TRUE(s5->GetType()->IsPrimitiveLong());
  // EXPECT_EQ(0x1234567890abcdefLL, s5->GetLong(NULL));  // TODO: needs clinit to be run?
  s5->SetLong(NULL, 0x34567890abcdef12LL);

  Field* s6 = statics->FindStaticField("s6", class_linker_->FindClass("F", class_loader));
  EXPECT_TRUE(s6->GetType()->IsPrimitiveFloat());
  // EXPECT_EQ(0.5, s6->GetFloat(NULL));  // TODO: needs clinit to be run?
  s6->SetFloat(NULL, 0.75);

  Field* s7 = statics->FindStaticField("s7", class_linker_->FindClass("D", class_loader));
  EXPECT_TRUE(s7->GetType()->IsPrimitiveDouble());
  // EXPECT_EQ(16777217, s7->GetDouble(NULL));  // TODO: needs clinit to be run?
  s7->SetDouble(NULL, 16777219);

  Field* s8 = statics->FindStaticField("s8", class_linker_->FindClass("Ljava/lang/Object;", class_loader));
  EXPECT_FALSE(s8->GetType()->IsPrimitive());
  // EXPECT_TRUE(s8->GetObject(NULL)->AsString()->Equals("android"));  // TODO: needs clinit to be run?
  s8->SetObject(NULL, String::AllocFromModifiedUtf8("robot"));

  Field* s9 = statics->FindStaticField("s9", class_linker_->FindClass("[Ljava/lang/Object;", class_loader));
  EXPECT_TRUE(s9->GetType()->IsArrayClass());
  // EXPECT_EQ(NULL, s9->GetObject(NULL));  // TODO: needs clinit to be run?
  s9->SetObject(NULL, NULL);

  EXPECT_EQ(false,                s0->GetBoolean(NULL));
  EXPECT_EQ(6,                    s1->GetByte(NULL));
  EXPECT_EQ('b',                  s2->GetChar(NULL));
  EXPECT_EQ(65001,                s3->GetShort(NULL));
  EXPECT_EQ(2000000001,           s4->GetInt(NULL));
  EXPECT_EQ(0x34567890abcdef12LL, s5->GetLong(NULL));
  EXPECT_EQ(0.75,                 s6->GetFloat(NULL));
  EXPECT_EQ(16777219,             s7->GetDouble(NULL));
  EXPECT_TRUE(s8->GetObject(NULL)->AsString()->Equals("robot"));
}

TEST_F(ClassLinkerTest, Interfaces) {
  const ClassLoader* class_loader = LoadDex("Interfaces");
  Class* I = class_linker_->FindClass("LInterfaces$I;", class_loader);
  Class* J = class_linker_->FindClass("LInterfaces$J;", class_loader);
  Class* K = class_linker_->FindClass("LInterfaces$K;", class_loader);
  Class* A = class_linker_->FindClass("LInterfaces$A;", class_loader);
  Class* B = class_linker_->FindClass("LInterfaces$B;", class_loader);
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
  EXPECT_EQ(Ai, A->FindVirtualMethodForInterface(Ii));
  EXPECT_EQ(Aj1, A->FindVirtualMethodForInterface(Jj1));
  EXPECT_EQ(Aj2, A->FindVirtualMethodForInterface(Jj2));
  EXPECT_EQ(Ai, A->FindVirtualMethodForVirtualOrInterface(Ii));
  EXPECT_EQ(Aj1, A->FindVirtualMethodForVirtualOrInterface(Jj1));
  EXPECT_EQ(Aj2, A->FindVirtualMethodForVirtualOrInterface(Jj2));
}

TEST_F(ClassLinkerTest, InitializeStaticStorageFromCode) {
  // pretend we are trying to get the static storage for the Statics class.

  // case 1, get the uninitialized storage from Statics.<clinit>
  // case 2, get the initialized storage from Statics.getS8

  const ClassLoader* class_loader = LoadDex("Statics");
  const DexFile* dex_file = ClassLoader::GetClassPath(class_loader)[0];
  CHECK(dex_file != NULL);

  Class* Statics = class_linker_->FindClass("LStatics;", class_loader);
  Method* clinit = Statics->FindDirectMethod("<clinit>", "()V");
  Method* getS8 = Statics->FindDirectMethod("getS8", "()Ljava/lang/Object;");
  uint32_t type_idx = FindTypeIdxByDescriptor(*dex_file, "LStatics;");

  EXPECT_TRUE(clinit->GetDexCacheInitializedStaticStorage()->Get(type_idx) == NULL);
  StaticStorageBase* uninit = class_linker_->InitializeStaticStorageFromCode(type_idx, clinit);
  EXPECT_TRUE(uninit != NULL);
  EXPECT_TRUE(clinit->GetDexCacheInitializedStaticStorage()->Get(type_idx) == NULL);
  StaticStorageBase* init = class_linker_->InitializeStaticStorageFromCode(type_idx, getS8);
  EXPECT_TRUE(init != NULL);
  EXPECT_EQ(init, clinit->GetDexCacheInitializedStaticStorage()->Get(type_idx));
}

}  // namespace art
