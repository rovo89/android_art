// Copyright 2011 Google Inc. All Rights Reserved.

#include "common_test.h"
#include "class_linker.h"
#include "dex_file.h"
#include "heap.h"
#include "stringprintf.h"
#include "gtest/gtest.h"

namespace art {

class ClassLinkerTest : public RuntimeTest {
 protected:
  void AssertNonExistantClass(const StringPiece& descriptor) {
    EXPECT_TRUE(class_linker_->FindSystemClass(descriptor) == NULL);
  }

  void AssertPrimitiveClass(const StringPiece& descriptor) {
    Class* primitive = class_linker_->FindSystemClass(descriptor);
    ASSERT_TRUE(primitive != NULL);
    ASSERT_TRUE(primitive->GetClass() != NULL);
    ASSERT_EQ(primitive->GetClass(), primitive->GetClass()->GetClass());
    EXPECT_TRUE(primitive->GetClass()->GetSuperClass() != NULL);
    ASSERT_EQ(descriptor, primitive->GetDescriptor());
    EXPECT_TRUE(primitive->GetSuperClass() == NULL);
    EXPECT_FALSE(primitive->HasSuperClass());
    EXPECT_TRUE(primitive->GetComponentType() == NULL);
    EXPECT_TRUE(primitive->GetStatus() == Class::kStatusInitialized);
    EXPECT_FALSE(primitive->IsErroneous());
    EXPECT_TRUE(primitive->IsVerified());
    EXPECT_TRUE(primitive->IsLinked());
    EXPECT_FALSE(primitive->IsArray());
    EXPECT_EQ(0, primitive->array_rank_);
    EXPECT_FALSE(primitive->IsInterface());
    EXPECT_TRUE(primitive->IsPublic());
    EXPECT_TRUE(primitive->IsFinal());
    EXPECT_TRUE(primitive->IsPrimitive());
    EXPECT_EQ(0U, primitive->NumDirectMethods());
    EXPECT_EQ(0U, primitive->NumVirtualMethods());
    EXPECT_EQ(0U, primitive->NumInstanceFields());
    EXPECT_EQ(0U, primitive->NumStaticFields());
    EXPECT_EQ(0U, primitive->NumInterfaces());
  }

  void AssertArrayClass(const StringPiece& array_descriptor,
                        int32_t array_rank,
                        const StringPiece& component_type) {
    Class* array = class_linker_->FindSystemClass(array_descriptor);
    ASSERT_TRUE(array != NULL);
    ASSERT_TRUE(array->GetClass() != NULL);
    ASSERT_EQ(array->GetClass(), array->GetClass()->GetClass());
    EXPECT_TRUE(array->GetClass()->GetSuperClass() != NULL);
    ASSERT_EQ(array_descriptor, array->GetDescriptor());
    EXPECT_TRUE(array->GetSuperClass() != NULL);
    EXPECT_EQ(class_linker_->FindSystemClass("Ljava/lang/Object;"), array->GetSuperClass());
    EXPECT_TRUE(array->HasSuperClass());
    ASSERT_TRUE(array->GetComponentType() != NULL);
    ASSERT_TRUE(array->GetComponentType()->GetDescriptor() != NULL);
    EXPECT_EQ(component_type, array->GetComponentType()->GetDescriptor());
    EXPECT_TRUE(array->GetStatus() == Class::kStatusInitialized);
    EXPECT_FALSE(array->IsErroneous());
    EXPECT_TRUE(array->IsVerified());
    EXPECT_TRUE(array->IsLinked());
    EXPECT_TRUE(array->IsArray());
    EXPECT_EQ(array_rank, array->array_rank_);
    EXPECT_FALSE(array->IsInterface());
    EXPECT_EQ(array->GetComponentType()->IsPublic(), array->IsPublic());
    EXPECT_TRUE(array->IsFinal());
    EXPECT_FALSE(array->IsPrimitive());
    EXPECT_EQ(0U, array->NumDirectMethods());
    EXPECT_EQ(0U, array->NumVirtualMethods());
    EXPECT_EQ(0U, array->NumInstanceFields());
    EXPECT_EQ(0U, array->NumStaticFields());
    EXPECT_EQ(2U, array->NumInterfaces());
  }

  void AssertDexFileClass(const DexFile* dex, const char* descriptor) {
    ASSERT_TRUE(descriptor != NULL);
    Class* klass = class_linker_->FindClass(descriptor, NULL, dex);
    ASSERT_TRUE(klass != NULL);
    EXPECT_EQ(descriptor, klass->GetDescriptor());
    if (klass->descriptor_ == "Ljava/lang/Object;") {
        EXPECT_FALSE(klass->HasSuperClass());
    } else {
        EXPECT_TRUE(klass->HasSuperClass());
        EXPECT_TRUE(klass->GetSuperClass() != NULL);
    }
    // EXPECT_TRUE(klass->GetClassLoader() != NULL); // TODO needs class loader
    EXPECT_TRUE(klass->GetDexCache() != NULL);
    EXPECT_TRUE(klass->GetComponentType() == NULL);
    EXPECT_TRUE(klass->GetComponentType() == NULL);
    EXPECT_EQ(Class::kStatusResolved, klass->GetStatus());
    EXPECT_FALSE(klass->IsErroneous());
    EXPECT_FALSE(klass->IsVerified());
    EXPECT_TRUE(klass->IsLinked());
    EXPECT_TRUE(klass->IsLoaded());
    EXPECT_TRUE(klass->IsInSamePackage(klass));
    EXPECT_TRUE(Class::IsInSamePackage(klass->GetDescriptor(), klass->GetDescriptor()));
    if (klass->IsInterface()) {
        EXPECT_TRUE(klass->IsAbstract());
        if (klass->NumDirectMethods() == 1) {
            EXPECT_EQ("<clinit>", klass->GetDirectMethod(0)->GetName());
        } else {
            EXPECT_EQ(0U, klass->NumDirectMethods());
        }
    } else {
        if (!klass->IsSynthetic()) {
            EXPECT_NE(0U, klass->NumDirectMethods());
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
        EXPECT_TRUE(method != NULL);
    }

    for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
        Method* method = klass->GetVirtualMethod(i);
        EXPECT_TRUE(method != NULL);
    }

    for (size_t i = 0; i < klass->NumInstanceFields(); i++) {
        InstanceField* field = klass->GetInstanceField(i);
        EXPECT_TRUE(field != NULL);
    }

    for (size_t i = 0; i < klass->NumStaticFields(); i++) {
        StaticField* field = klass->GetStaticField(i);
        EXPECT_TRUE(field != NULL);
    }

    // Confirm that all instances fields are packed together at the start
    EXPECT_GE(klass->NumInstanceFields(), klass->NumReferenceInstanceFields());
    for (size_t i = 0; i < klass->NumReferenceInstanceFields(); i++) {
        InstanceField* field = klass->GetInstanceField(i);
        ASSERT_TRUE(field != NULL);
        ASSERT_TRUE(field->GetDescriptor() != NULL);
        Class* fieldType = class_linker_->FindClass(field->GetDescriptor(), NULL, dex);
        ASSERT_TRUE(fieldType != NULL);
        EXPECT_FALSE(fieldType->IsPrimitive());
    }
    for (size_t i = klass->NumReferenceInstanceFields(); i < klass->NumInstanceFields(); i++) {
        InstanceField* field = klass->GetInstanceField(i);
        ASSERT_TRUE(field != NULL);
        ASSERT_TRUE(field->GetDescriptor() != NULL);
        Class* fieldType = class_linker_->FindClass(field->GetDescriptor(), NULL, dex);
        ASSERT_TRUE(fieldType != NULL);
        EXPECT_TRUE(fieldType->IsPrimitive());
    }

    size_t total_num_reference_instance_fields = 0;
    Class* k = klass;
    while (k != NULL) {
        total_num_reference_instance_fields += k->NumReferenceInstanceFields();
        k = k->GetSuperClass();
    }
    EXPECT_EQ(klass->GetReferenceOffsets() == 0,
              total_num_reference_instance_fields == 0);
  }

  void AssertDexFile(const DexFile* dex) {
    ASSERT_TRUE(dex != NULL);
    class_linker_->RegisterDexFile(dex);
    for (size_t i = 0; i < dex->NumClassDefs(); i++) {
      const DexFile::ClassDef class_def = dex->GetClassDef(i);
      const char* descriptor = dex->GetClassDescriptor(class_def);
      AssertDexFileClass(dex, descriptor);
    }
  }

};

TEST_F(ClassLinkerTest, FindClassNonexistent) {
  Class* result1 = class_linker_->FindSystemClass("NoSuchClass;");
  EXPECT_TRUE(result1 == NULL);
  Class* result2 = class_linker_->FindSystemClass("LNoSuchClass;");
  EXPECT_TRUE(result2 == NULL);
}

TEST_F(ClassLinkerTest, FindClassNested) {
  scoped_ptr<DexFile> nested_dex(OpenDexFileBase64(kNestedDex));
  class_linker_->RegisterDexFile(nested_dex.get());

  Class* outer = class_linker_->FindClass("LNested;", NULL, nested_dex.get());
  ASSERT_TRUE(outer != NULL);
  EXPECT_EQ(0U, outer->NumVirtualMethods());
  EXPECT_EQ(1U, outer->NumDirectMethods());

  Class* inner = class_linker_->FindClass("LNested$Inner;", NULL, nested_dex.get());
  ASSERT_TRUE(inner != NULL);
  EXPECT_EQ(0U, inner->NumVirtualMethods());
  EXPECT_EQ(1U, inner->NumDirectMethods());
}

TEST_F(ClassLinkerTest, FindClass) {
  ClassLinker* linker = class_linker_;

  StringPiece expected = "BCDFIJSZV";
  for (int ch = 0; ch < 255; ch++) {
    char* s = reinterpret_cast<char*>(&ch);
    StringPiece descriptor(s, 1);
    if (expected.find(ch) == StringPiece::npos) {
      AssertNonExistantClass(descriptor);
    } else {
      AssertPrimitiveClass(descriptor);
    }
  }

  Class* JavaLangObject = linker->FindSystemClass("Ljava/lang/Object;");
  ASSERT_TRUE(JavaLangObject != NULL);
  ASSERT_TRUE(JavaLangObject->GetClass() != NULL);
  ASSERT_EQ(JavaLangObject->GetClass(), JavaLangObject->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, JavaLangObject->GetClass()->GetSuperClass());
  ASSERT_TRUE(JavaLangObject->GetDescriptor() == "Ljava/lang/Object;");
  EXPECT_TRUE(JavaLangObject->GetSuperClass() == NULL);
  EXPECT_FALSE(JavaLangObject->HasSuperClass());
  EXPECT_TRUE(JavaLangObject->GetComponentType() == NULL);
  EXPECT_FALSE(JavaLangObject->IsErroneous());
  EXPECT_FALSE(JavaLangObject->IsVerified());
  EXPECT_TRUE(JavaLangObject->IsLinked());
  EXPECT_FALSE(JavaLangObject->IsArray());
  EXPECT_EQ(0, JavaLangObject->array_rank_);
  EXPECT_FALSE(JavaLangObject->IsInterface());
  EXPECT_TRUE(JavaLangObject->IsPublic());
  EXPECT_FALSE(JavaLangObject->IsFinal());
  EXPECT_FALSE(JavaLangObject->IsPrimitive());
  EXPECT_EQ(1U, JavaLangObject->NumDirectMethods());
  EXPECT_EQ(0U, JavaLangObject->NumVirtualMethods());
  EXPECT_EQ(0U, JavaLangObject->NumInstanceFields());
  EXPECT_EQ(0U, JavaLangObject->NumStaticFields());
  EXPECT_EQ(0U, JavaLangObject->NumInterfaces());


  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassDex));
  linker->RegisterDexFile(dex.get());
  EXPECT_TRUE(linker->FindSystemClass("LMyClass;") == NULL);
  Class* MyClass = linker->FindClass("LMyClass;", NULL, dex.get());
  ASSERT_TRUE(MyClass != NULL);
  ASSERT_TRUE(MyClass->GetClass() != NULL);
  ASSERT_EQ(MyClass->GetClass(), MyClass->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, MyClass->GetClass()->GetSuperClass());
  ASSERT_TRUE(MyClass->GetDescriptor() == "LMyClass;");
  EXPECT_TRUE(MyClass->GetSuperClass() == JavaLangObject);
  EXPECT_TRUE(MyClass->HasSuperClass());
  EXPECT_TRUE(MyClass->GetComponentType() == NULL);
  EXPECT_TRUE(MyClass->GetStatus() == Class::kStatusResolved);
  EXPECT_FALSE(MyClass->IsErroneous());
  EXPECT_FALSE(MyClass->IsVerified());
  EXPECT_TRUE(MyClass->IsLinked());
  EXPECT_FALSE(MyClass->IsArray());
  EXPECT_EQ(0, JavaLangObject->array_rank_);
  EXPECT_FALSE(MyClass->IsInterface());
  EXPECT_FALSE(MyClass->IsPublic());
  EXPECT_FALSE(MyClass->IsFinal());
  EXPECT_FALSE(MyClass->IsPrimitive());
  EXPECT_EQ(1U, MyClass->NumDirectMethods());
  EXPECT_EQ(0U, MyClass->NumVirtualMethods());
  EXPECT_EQ(0U, MyClass->NumInstanceFields());
  EXPECT_EQ(0U, MyClass->NumStaticFields());
  EXPECT_EQ(0U, MyClass->NumInterfaces());

  EXPECT_EQ(JavaLangObject->GetClass()->GetClass(), MyClass->GetClass()->GetClass());

  // created by class_linker
  AssertArrayClass("[C", 1, "C");
  AssertArrayClass("[Ljava/lang/Object;", 1, "Ljava/lang/Object;");
  // synthesized on the fly
  AssertArrayClass("[[C", 2, "C");
  AssertArrayClass("[[[LMyClass;", 3, "LMyClass;");
  // or not available at all
  AssertNonExistantClass("[[[[LNonExistantClass;");
}

TEST_F(ClassLinkerTest, ProtoCompare) {
  ClassLinker* linker = class_linker_;

  scoped_ptr<DexFile> proto_dex_file(OpenDexFileBase64(kProtoCompareDex));
  linker->RegisterDexFile(proto_dex_file.get());

  Class* klass = linker->FindClass("LProtoCompare;", NULL, proto_dex_file.get());
  ASSERT_TRUE(klass != NULL);

  ASSERT_EQ(4U, klass->NumVirtualMethods());

  Method* m1 = klass->GetVirtualMethod(0);
  ASSERT_EQ("m1", m1->GetName());

  Method* m2 = klass->GetVirtualMethod(1);
  ASSERT_EQ("m2", m2->GetName());

  Method* m3 = klass->GetVirtualMethod(2);
  ASSERT_EQ("m3", m3->GetName());

  Method* m4 = klass->GetVirtualMethod(3);
  ASSERT_EQ("m4", m4->GetName());

  EXPECT_TRUE(linker->HasSameReturnType(m1, m2));
  EXPECT_TRUE(linker->HasSameReturnType(m2, m1));

  EXPECT_TRUE(linker->HasSameReturnType(m1, m2));
  EXPECT_TRUE(linker->HasSameReturnType(m2, m1));

  EXPECT_FALSE(linker->HasSameReturnType(m1, m4));
  EXPECT_FALSE(linker->HasSameReturnType(m4, m1));

  EXPECT_TRUE(linker->HasSameArgumentTypes(m1, m2));
  EXPECT_TRUE(linker->HasSameArgumentTypes(m2, m1));

  EXPECT_FALSE(linker->HasSameArgumentTypes(m1, m3));
  EXPECT_FALSE(linker->HasSameArgumentTypes(m3, m1));

  EXPECT_FALSE(linker->HasSameArgumentTypes(m1, m4));
  EXPECT_FALSE(linker->HasSameArgumentTypes(m4, m1));

  EXPECT_TRUE(linker->HasSamePrototype(m1, m2));
  EXPECT_TRUE(linker->HasSamePrototype(m2, m1));

  EXPECT_FALSE(linker->HasSamePrototype(m1, m3));
  EXPECT_FALSE(linker->HasSamePrototype(m3, m1));

  EXPECT_FALSE(linker->HasSamePrototype(m3, m4));
  EXPECT_FALSE(linker->HasSamePrototype(m4, m3));

  EXPECT_FALSE(linker->HasSameName(m1, m2));
  EXPECT_FALSE(linker->HasSameNameAndPrototype(m1, m2));
}

TEST_F(ClassLinkerTest, ProtoCompare2) {
  ClassLinker* linker = class_linker_;

  scoped_ptr<DexFile> proto1_dex_file(OpenDexFileBase64(kProtoCompareDex));
  linker->RegisterDexFile(proto1_dex_file.get());
  scoped_ptr<DexFile> proto2_dex_file(OpenDexFileBase64(kProtoCompare2Dex));
  linker->RegisterDexFile(proto2_dex_file.get());

  Class* klass1 = linker->FindClass("LProtoCompare;", NULL, proto1_dex_file.get());
  ASSERT_TRUE(klass1 != NULL);
  Class* klass2 = linker->FindClass("LProtoCompare2;", NULL, proto2_dex_file.get());
  ASSERT_TRUE(klass2 != NULL);

  Method* m1_1 = klass1->GetVirtualMethod(0);
  ASSERT_EQ("m1", m1_1->GetName());
  Method* m2_1 = klass1->GetVirtualMethod(1);
  ASSERT_EQ("m2", m2_1->GetName());
  Method* m3_1 = klass1->GetVirtualMethod(2);
  ASSERT_EQ("m3", m3_1->GetName());
  Method* m4_1 = klass1->GetVirtualMethod(3);
  ASSERT_EQ("m4", m4_1->GetName());

  Method* m1_2 = klass2->GetVirtualMethod(0);
  ASSERT_EQ("m1", m1_2->GetName());
  Method* m2_2 = klass2->GetVirtualMethod(1);
  ASSERT_EQ("m2", m2_2->GetName());
  Method* m3_2 = klass2->GetVirtualMethod(2);
  ASSERT_EQ("m3", m3_2->GetName());
  Method* m4_2 = klass2->GetVirtualMethod(3);
  ASSERT_EQ("m4", m4_2->GetName());

  EXPECT_TRUE(linker->HasSameNameAndPrototype(m1_1, m1_2));
  EXPECT_TRUE(linker->HasSameNameAndPrototype(m1_2, m1_1));

  EXPECT_TRUE(linker->HasSameNameAndPrototype(m2_1, m2_2));
  EXPECT_TRUE(linker->HasSameNameAndPrototype(m2_2, m2_1));

  EXPECT_TRUE(linker->HasSameNameAndPrototype(m3_1, m3_2));
  EXPECT_TRUE(linker->HasSameNameAndPrototype(m3_2, m3_1));

  EXPECT_TRUE(linker->HasSameNameAndPrototype(m4_1, m4_2));
  EXPECT_TRUE(linker->HasSameNameAndPrototype(m4_2, m4_1));
}

TEST_F(ClassLinkerTest, LibCore) {
  // TODO add host support when we have DexFile::OpenJar
  if (!is_host_) {
    return;
  }

  // TODO switch to jar when we have DexFile::OpenJar
  std::string libcore_dex_file_name = StringPrintf("%s/out/target/common/obj/JAVA_LIBRARIES/core_intermediates/noproguard.classes.dex",
                                                   getenv("ANDROID_BUILD_TOP"));
  scoped_ptr<DexFile> libcore_dex_file(DexFile::OpenFile(libcore_dex_file_name.c_str()));
  AssertDexFile(libcore_dex_file.get());
}

}  // namespace art
