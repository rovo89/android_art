// Copyright 2011 Google Inc. All Rights Reserved.

#include "common_test.h"
#include "class_linker.h"
#include "dex_file.h"
#include "heap.h"
#include "gtest/gtest.h"

namespace art {

class ClassLinkerTest : public RuntimeTest {
 protected:
  void AssertNonExistantClass(const StringPiece& descriptor) {
    EXPECT_TRUE(class_linker_.get()->FindSystemClass(descriptor) == NULL);
  }

  void AssertPrimitiveClass(const StringPiece& descriptor) {
    Class* primitive = class_linker_.get()->FindSystemClass(descriptor);
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
    Class* array = class_linker_.get()->FindSystemClass(array_descriptor);
    ASSERT_TRUE(array != NULL);
    ASSERT_TRUE(array->GetClass() != NULL);
    ASSERT_EQ(array->GetClass(), array->GetClass()->GetClass());
    EXPECT_TRUE(array->GetClass()->GetSuperClass() != NULL);
    ASSERT_EQ(array_descriptor, array->GetDescriptor());
    EXPECT_TRUE(array->GetSuperClass() != NULL);
    EXPECT_EQ(class_linker_.get()->FindSystemClass("Ljava/lang/Object;"), array->GetSuperClass());
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
};

TEST_F(ClassLinkerTest, FindClassNonexistent) {
  Class* result1 = class_linker_.get()->FindSystemClass("NoSuchClass;");
  EXPECT_TRUE(result1 == NULL);
  Class* result2 = class_linker_.get()->FindSystemClass("LNoSuchClass;");
  EXPECT_TRUE(result2 == NULL);
}

TEST_F(ClassLinkerTest, FindClassNested) {
  scoped_ptr<DexFile> nested_dex(OpenDexFileBase64(kNestedDex));
  class_linker_.get()->RegisterDexFile(nested_dex.get());

  Class* outer = class_linker_.get()->FindClass("LNested;", NULL, nested_dex.get());
  ASSERT_TRUE(outer != NULL);
  EXPECT_EQ(0U, outer->NumVirtualMethods());
  EXPECT_EQ(1U, outer->NumDirectMethods());

  Class* inner = class_linker_.get()->FindClass("LNested$Inner;", NULL, nested_dex.get());
  ASSERT_TRUE(inner != NULL);
  EXPECT_EQ(0U, inner->NumVirtualMethods());
  EXPECT_EQ(1U, inner->NumDirectMethods());
}

TEST_F(ClassLinkerTest, FindClass) {
  ClassLinker* linker = class_linker_.get();

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
  ClassLinker* linker = class_linker_.get();

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
  ClassLinker* linker = class_linker_.get();

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

}  // namespace art
