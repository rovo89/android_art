// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/common_test.h"
#include "src/class_linker.h"
#include "src/dex_file.h"
#include "src/heap.h"
#include "gtest/gtest.h"

namespace art {

class ClassLinkerTest : public RuntimeTest {};

TEST_F(ClassLinkerTest, FindClassNonexistent) {
  scoped_ptr<DexFile> dex(DexFile::OpenBase64(kMyClassDex));
  ASSERT_TRUE(dex != NULL);

  scoped_ptr<ClassLinker> linker(ClassLinker::Create());
  linker->AppendToClassPath(dex.get());

  Class* result1 = linker.get()->FindClass("NoSuchClass;", NULL);
  EXPECT_TRUE(result1 == NULL);
  Class* result2 = linker.get()->FindClass("LNoSuchClass;", NULL);
  EXPECT_TRUE(result2 == NULL);
}

TEST_F(ClassLinkerTest, FindClassNested) {
  scoped_ptr<DexFile> objectDex(DexFile::OpenBase64(kJavaLangDex));
  ASSERT_TRUE(objectDex != NULL);
  scoped_ptr<DexFile> nestedDex(DexFile::OpenBase64(kNestedDex));
  ASSERT_TRUE(nestedDex != NULL);

  scoped_ptr<ClassLinker> linker(ClassLinker::Create());
  linker->AppendToClassPath(objectDex.get());
  linker->AppendToClassPath(nestedDex.get());

  Class* outer = linker.get()->FindClass("LNested;", NULL);
  ASSERT_TRUE(outer != NULL);
  EXPECT_EQ(0U, outer->NumVirtualMethods());
  EXPECT_EQ(1U, outer->NumDirectMethods());

  Class* inner = linker.get()->FindClass("LNested$Inner;", NULL);
  ASSERT_TRUE(inner != NULL);
  EXPECT_EQ(0U, inner->NumVirtualMethods());
  EXPECT_EQ(1U, inner->NumDirectMethods());
}

static void AssertNonExistantClass(ClassLinker* linker, const StringPiece& descriptor) {
  EXPECT_TRUE(linker->FindClass(descriptor, NULL) == NULL);
}

static void AssertPrimitiveClass(ClassLinker* linker, const StringPiece& descriptor) {
  Class* primitive = linker->FindClass(descriptor, NULL);
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
  EXPECT_EQ(0U, primitive->interface_count_);
}

static void AssertArrayClass(ClassLinker* linker,
                             const StringPiece& array_descriptor,
                             int32_t array_rank,
                             const StringPiece& component_type) {
  Class* array = linker->FindClass(array_descriptor, NULL);
  ASSERT_TRUE(array != NULL);
  ASSERT_TRUE(array->GetClass() != NULL);
  ASSERT_EQ(array->GetClass(), array->GetClass()->GetClass());
  EXPECT_TRUE(array->GetClass()->GetSuperClass() != NULL);
  ASSERT_EQ(array_descriptor, array->GetDescriptor());
  EXPECT_TRUE(array->GetSuperClass() != NULL);
  EXPECT_EQ(linker->FindSystemClass("Ljava/lang/Object;"), array->GetSuperClass());
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
  EXPECT_EQ(2U, array->interface_count_);
}

TEST_F(ClassLinkerTest, FindClass) {
  scoped_ptr<ClassLinker> linker(ClassLinker::Create());

  StringPiece expected = "BCDFIJSZV";
  for (int ch = 0; ch < 255; ch++) {
    char* s = reinterpret_cast<char*>(&ch);
    StringPiece descriptor(s, 1);
    if (expected.find(ch) == StringPiece::npos) {
      AssertNonExistantClass(linker.get(), descriptor);
    } else {
      AssertPrimitiveClass(linker.get(), descriptor);
    }
  }

  scoped_ptr<DexFile> dex(DexFile::OpenBase64(kMyClassDex));
  ASSERT_TRUE(dex != NULL);
  linker->AppendToClassPath(dex.get());

  Class* JavaLangObject = linker->FindClass("Ljava/lang/Object;", NULL);
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
  EXPECT_EQ(0U, JavaLangObject->interface_count_);


  Class* MyClass = linker->FindClass("LMyClass;", NULL);
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
  EXPECT_EQ(0U, MyClass->interface_count_);

  EXPECT_EQ(JavaLangObject->GetClass()->GetClass(), MyClass->GetClass()->GetClass());

  // created by class_linker
  AssertArrayClass(linker.get(), "[C", 1, "C");
  // synthesized on the fly
  AssertArrayClass(linker.get(), "[[C", 2, "C");
  AssertArrayClass(linker.get(), "[[[LMyClass;", 3, "LMyClass;");
  // or not available at all
  AssertNonExistantClass(linker.get(), "[[[[LNonExistantClass;");
}

}  // namespace art
