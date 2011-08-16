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
    EXPECT_TRUE(primitive->GetClassLoader() == NULL);
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
    EXPECT_FALSE(primitive->IsSynthetic());
    EXPECT_EQ(0U, primitive->NumDirectMethods());
    EXPECT_EQ(0U, primitive->NumVirtualMethods());
    EXPECT_EQ(0U, primitive->NumInstanceFields());
    EXPECT_EQ(0U, primitive->NumStaticFields());
    EXPECT_EQ(0U, primitive->NumInterfaces());
  }

  void AssertArrayClass(const StringPiece& array_descriptor,
                        int32_t array_rank,
                        const StringPiece& component_type,
                        ClassLoader* class_loader) {
    Class* array = class_linker_->FindClass(array_descriptor, class_loader);
    ASSERT_TRUE(array != NULL);
    ASSERT_TRUE(array->GetClass() != NULL);
    ASSERT_EQ(array->GetClass(), array->GetClass()->GetClass());
    EXPECT_TRUE(array->GetClass()->GetSuperClass() != NULL);
    ASSERT_EQ(array_descriptor, array->GetDescriptor());
    EXPECT_TRUE(array->GetSuperClass() != NULL);
    EXPECT_EQ(class_linker_->FindSystemClass("Ljava/lang/Object;"), array->GetSuperClass());
    EXPECT_TRUE(array->HasSuperClass());
    EXPECT_EQ(class_loader, array->GetClassLoader());
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
    EXPECT_FALSE(array->IsSynthetic());
    EXPECT_EQ(0U, array->NumDirectMethods());
    EXPECT_EQ(0U, array->NumVirtualMethods());
    EXPECT_EQ(0U, array->NumInstanceFields());
    EXPECT_EQ(0U, array->NumStaticFields());
    EXPECT_EQ(2U, array->NumInterfaces());
  }

  void AssertDexFileClass(ClassLoader* class_loader, const char* descriptor) {
    ASSERT_TRUE(descriptor != NULL);
    Class* klass = class_linker_->FindSystemClass(descriptor);
    ASSERT_TRUE(klass != NULL);
    EXPECT_EQ(descriptor, klass->GetDescriptor());
    if (klass->descriptor_ == "Ljava/lang/Object;") {
      EXPECT_FALSE(klass->HasSuperClass());
    } else {
      EXPECT_TRUE(klass->HasSuperClass());
      EXPECT_TRUE(klass->GetSuperClass() != NULL);
    }
    EXPECT_EQ(class_loader, klass->GetClassLoader());
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
        EXPECT_TRUE(klass->GetDirectMethod(0)->GetName()->Equals("<clinit>"));
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
      EXPECT_EQ(klass, method->GetDeclaringClass());
    }

    for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
      Method* method = klass->GetVirtualMethod(i);
      EXPECT_TRUE(method != NULL);
    }

    for (size_t i = 0; i < klass->NumInstanceFields(); i++) {
      Field* field = klass->GetInstanceField(i);
      EXPECT_TRUE(field != NULL);
      EXPECT_FALSE(field->IsStatic());
      EXPECT_EQ(klass, field->GetDeclaringClass());
    }

    for (size_t i = 0; i < klass->NumStaticFields(); i++) {
      Field* field = klass->GetStaticField(i);
      EXPECT_TRUE(field != NULL);
      EXPECT_TRUE(field->IsStatic());
      EXPECT_EQ(klass, field->GetDeclaringClass());
    }

    // Confirm that all instances fields are packed together at the start
    EXPECT_GE(klass->NumInstanceFields(), klass->NumReferenceInstanceFields());
    for (size_t i = 0; i < klass->NumReferenceInstanceFields(); i++) {
      Field* field = klass->GetInstanceField(i);
      ASSERT_TRUE(field != NULL);
      ASSERT_TRUE(field->GetDescriptor() != NULL);
      Class* field_type = class_linker_->FindClass(field->GetDescriptor(), class_loader);
      ASSERT_TRUE(field_type != NULL);
      EXPECT_FALSE(field_type->IsPrimitive());
    }
    for (size_t i = klass->NumReferenceInstanceFields(); i < klass->NumInstanceFields(); i++) {
      Field* field = klass->GetInstanceField(i);
      ASSERT_TRUE(field != NULL);
      ASSERT_TRUE(field->GetDescriptor() != NULL);
      Class* field_type = class_linker_->FindClass(field->GetDescriptor(), class_loader);
      ASSERT_TRUE(field_type != NULL);
      EXPECT_TRUE(field_type->IsPrimitive());
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

  static void TestRootVisitor(Object* root, void* arg) {
    EXPECT_TRUE(root != NULL);
  }

  void AssertDexFile(const DexFile* dex, ClassLoader* class_loader) {
    ASSERT_TRUE(dex != NULL);
    for (size_t i = 0; i < dex->NumClassDefs(); i++) {
      const DexFile::ClassDef class_def = dex->GetClassDef(i);
      const char* descriptor = dex->GetClassDescriptor(class_def);
      AssertDexFileClass(class_loader, descriptor);
    }
    class_linker_->VisitRoots(TestRootVisitor, NULL);
  }
};

TEST_F(ClassLinkerTest, FindClassNonexistent) {
  Class* result1 = class_linker_->FindSystemClass("NoSuchClass;");
  EXPECT_TRUE(result1 == NULL);
  Class* result2 = class_linker_->FindSystemClass("LNoSuchClass;");
  EXPECT_TRUE(result2 == NULL);
}

TEST_F(ClassLinkerTest, FindClassNested) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kNestedDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());

  Class* outer = class_linker_->FindClass("LNested;", class_loader);
  ASSERT_TRUE(outer != NULL);
  EXPECT_EQ(0U, outer->NumVirtualMethods());
  EXPECT_EQ(1U, outer->NumDirectMethods());

  Class* inner = class_linker_->FindClass("LNested$Inner;", class_loader);
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
  EXPECT_TRUE(JavaLangObject->GetClassLoader() == NULL);
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
  EXPECT_FALSE(JavaLangObject->IsSynthetic());
  EXPECT_EQ(2U, JavaLangObject->NumDirectMethods());
  EXPECT_EQ(11U, JavaLangObject->NumVirtualMethods());
  EXPECT_EQ(0U, JavaLangObject->NumInstanceFields());
  EXPECT_EQ(0U, JavaLangObject->NumStaticFields());
  EXPECT_EQ(0U, JavaLangObject->NumInterfaces());


  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  EXPECT_TRUE(linker->FindSystemClass("LMyClass;") == NULL);
  Class* MyClass = linker->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(MyClass != NULL);
  ASSERT_TRUE(MyClass->GetClass() != NULL);
  ASSERT_EQ(MyClass->GetClass(), MyClass->GetClass()->GetClass());
  EXPECT_EQ(JavaLangObject, MyClass->GetClass()->GetSuperClass());
  ASSERT_TRUE(MyClass->GetDescriptor() == "LMyClass;");
  EXPECT_TRUE(MyClass->GetSuperClass() == JavaLangObject);
  EXPECT_TRUE(MyClass->HasSuperClass());
  EXPECT_EQ(class_loader, MyClass->GetClassLoader());
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
  EXPECT_FALSE(MyClass->IsSynthetic());
  EXPECT_EQ(1U, MyClass->NumDirectMethods());
  EXPECT_EQ(0U, MyClass->NumVirtualMethods());
  EXPECT_EQ(0U, MyClass->NumInstanceFields());
  EXPECT_EQ(0U, MyClass->NumStaticFields());
  EXPECT_EQ(0U, MyClass->NumInterfaces());

  EXPECT_EQ(JavaLangObject->GetClass()->GetClass(), MyClass->GetClass()->GetClass());

  // created by class_linker
  AssertArrayClass("[C", 1, "C", NULL);
  AssertArrayClass("[Ljava/lang/Object;", 1, "Ljava/lang/Object;", NULL);
  // synthesized on the fly
  AssertArrayClass("[[C", 2, "C", NULL);
  AssertArrayClass("[[[LMyClass;", 3, "LMyClass;", class_loader);
  // or not available at all
  AssertNonExistantClass("[[[[LNonExistantClass;");
}

TEST_F(ClassLinkerTest, LibCore) {
  scoped_ptr<DexFile> libcore_dex_file(GetLibCoreDex());
  EXPECT_TRUE(libcore_dex_file.get() != NULL);
  AssertDexFile(libcore_dex_file.get(), NULL);
}

// C++ fields must exactly match the fields in the Java classes. If this fails,
// reorder the fields in the C++ class. Managed class fields are ordered by
// ClassLinker::LinkInstanceFields.
TEST_F(ClassLinkerTest, ValidateFieldOrderOfJavaCppUnionClasses) {
  Class* string = class_linker_->FindSystemClass( "Ljava/lang/String;");
  ASSERT_EQ(4U, string->NumInstanceFields());
  EXPECT_TRUE(string->GetInstanceField(0)->GetName()->Equals("value"));
  EXPECT_TRUE(string->GetInstanceField(1)->GetName()->Equals("hashCode"));
  EXPECT_TRUE(string->GetInstanceField(2)->GetName()->Equals("offset"));
  EXPECT_TRUE(string->GetInstanceField(3)->GetName()->Equals("count"));

  Class* accessible_object = class_linker_->FindSystemClass("Ljava/lang/reflect/AccessibleObject;");
  ASSERT_EQ(1U, accessible_object->NumInstanceFields());
  EXPECT_TRUE(accessible_object->GetInstanceField(0)->GetName()->Equals("flag"));

  Class* field = class_linker_->FindSystemClass("Ljava/lang/reflect/Field;");
  ASSERT_EQ(6U, field->NumInstanceFields());
  EXPECT_TRUE(field->GetInstanceField(0)->GetName()->Equals("declaringClass"));
  EXPECT_TRUE(field->GetInstanceField(1)->GetName()->Equals("genericType"));
  EXPECT_TRUE(field->GetInstanceField(2)->GetName()->Equals("type"));
  EXPECT_TRUE(field->GetInstanceField(3)->GetName()->Equals("name"));
  EXPECT_TRUE(field->GetInstanceField(4)->GetName()->Equals("slot"));
  EXPECT_TRUE(field->GetInstanceField(5)->GetName()->Equals("genericTypesAreInitialized"));

  Class* method = class_linker_->FindSystemClass("Ljava/lang/reflect/Method;");
  ASSERT_EQ(11U, method->NumInstanceFields());
  EXPECT_TRUE(method->GetInstanceField( 0)->GetName()->Equals("declaringClass"));
  EXPECT_TRUE(method->GetInstanceField( 1)->GetName()->Equals("exceptionTypes"));
  EXPECT_TRUE(method->GetInstanceField( 2)->GetName()->Equals("formalTypeParameters"));
  EXPECT_TRUE(method->GetInstanceField( 3)->GetName()->Equals("genericExceptionTypes"));
  EXPECT_TRUE(method->GetInstanceField( 4)->GetName()->Equals("genericParameterTypes"));
  EXPECT_TRUE(method->GetInstanceField( 5)->GetName()->Equals("genericReturnType"));
  EXPECT_TRUE(method->GetInstanceField( 6)->GetName()->Equals("returnType"));
  EXPECT_TRUE(method->GetInstanceField( 7)->GetName()->Equals("name"));
  EXPECT_TRUE(method->GetInstanceField( 8)->GetName()->Equals("parameterTypes"));
  EXPECT_TRUE(method->GetInstanceField( 9)->GetName()->Equals("genericTypesAreInitialized"));
  EXPECT_TRUE(method->GetInstanceField(10)->GetName()->Equals("slot"));

  Class* class_loader = class_linker_->FindSystemClass("Ljava/lang/ClassLoader;");
  ASSERT_EQ(2U, class_loader->NumInstanceFields());
  EXPECT_TRUE(class_loader->GetInstanceField(0)->GetName()->Equals("packages"));
  EXPECT_TRUE(class_loader->GetInstanceField(1)->GetName()->Equals("parent"));

  Class* dex_base_class_loader = class_linker_->FindSystemClass("Ldalvik/system/BaseDexClassLoader;");
  ASSERT_EQ(2U, dex_base_class_loader->NumInstanceFields());
  EXPECT_TRUE(dex_base_class_loader->GetInstanceField(0)->GetName()->Equals("originalPath"));
  EXPECT_TRUE(dex_base_class_loader->GetInstanceField(1)->GetName()->Equals("pathList"));
}

// The first reference array element must be a multiple of 8 bytes from the
// start of the object
TEST_F(ClassLinkerTest, ValidateObjectArrayElementsOffset) {
  Class* array_class = class_linker_->FindSystemClass("[Ljava/lang/String;");
  ObjectArray<String>* array = ObjectArray<String>::Alloc(array_class, 0);
  uint32_t array_offset = reinterpret_cast<uint32_t>(array);
  uint32_t data_offset = reinterpret_cast<uint32_t>(array->GetData());
  EXPECT_EQ(16U, data_offset - array_offset);
}

TEST_F(ClassLinkerTest, ValidatePrimitiveArrayElementsOffset) {
  LongArray* array = LongArray::Alloc(0);
  EXPECT_EQ(class_linker_->FindSystemClass("[J"), array->GetClass());
  uint32_t array_offset = reinterpret_cast<uint32_t>(array);
  uint32_t data_offset = reinterpret_cast<uint32_t>(array->GetData());
  EXPECT_EQ(16U, data_offset - array_offset);
}

TEST_F(ClassLinkerTest, TwoClassLoadersOneClass) {
  scoped_ptr<DexFile> dex_1(OpenDexFileBase64(kMyClassDex));
  scoped_ptr<DexFile> dex_2(OpenDexFileBase64(kMyClassDex));
  PathClassLoader* class_loader_1 = AllocPathClassLoader(dex_1.get());
  PathClassLoader* class_loader_2 = AllocPathClassLoader(dex_2.get());
  Class* MyClass_1 = class_linker_->FindClass("LMyClass;", class_loader_1);
  Class* MyClass_2 = class_linker_->FindClass("LMyClass;", class_loader_2);
  EXPECT_TRUE(MyClass_1 != NULL);
  EXPECT_TRUE(MyClass_2 != NULL);
  EXPECT_NE(MyClass_1, MyClass_2);
}

TEST_F(ClassLinkerTest, StaticFields) {
  // TODO: uncomment expectations of initial values when InitializeClass works
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kStatics));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  Class* statics = class_linker_->FindClass("LStatics;", class_loader);
  // class_linker_->InitializeClass(statics);  // TODO uncomment this

  EXPECT_EQ(10U, statics->NumStaticFields());

  Field* s0 = statics->GetStaticField(0);
  EXPECT_EQ("Ljava/lang/reflect/Field;", s0->GetClass()->descriptor_);
  EXPECT_EQ('Z', s0->GetType());
//  EXPECT_EQ(true, s0->GetBoolean());  // TODO uncomment this
  s0->SetBoolean(false);

  Field* s1 = statics->GetStaticField(1);
  EXPECT_EQ('B', s1->GetType());
//  EXPECT_EQ(5, s1->GetByte());  // TODO uncomment this
  s1->SetByte(6);

  Field* s2 = statics->GetStaticField(2);
  EXPECT_EQ('C', s2->GetType());
//  EXPECT_EQ('a', s2->GetChar());  // TODO uncomment this
  s2->SetChar('b');

  Field* s3 = statics->GetStaticField(3);
  EXPECT_EQ('S', s3->GetType());
//  EXPECT_EQ(65000, s3->GetShort());  // TODO uncomment this
  s3->SetShort(65001);

  Field* s4 = statics->GetStaticField(4);
  EXPECT_EQ('I', s4->GetType());
//  EXPECT_EQ(2000000000, s4->GetInt());  // TODO uncomment this
  s4->SetInt(2000000001);

  Field* s5 = statics->GetStaticField(5);
  EXPECT_EQ('J', s5->GetType());
//  EXPECT_EQ(0x1234567890abcdefLL, s5->GetLong());  // TODO uncomment this
  s5->SetLong(0x34567890abcdef12LL);

  Field* s6 = statics->GetStaticField(6);
  EXPECT_EQ('F', s6->GetType());
//  EXPECT_EQ(0.5, s6->GetFloat());  // TODO uncomment this
  s6->SetFloat(0.75);

  Field* s7 = statics->GetStaticField(7);
  EXPECT_EQ('D', s7->GetType());
//  EXPECT_EQ(16777217, s7->GetDouble());  // TODO uncomment this
  s7->SetDouble(16777219);

  Field* s8 = statics->GetStaticField(8);
  EXPECT_EQ('L', s8->GetType());
//  EXPECT_TRUE(down_cast<String*>(s8->GetObject())->Equals("android"));  // TODO uncomment this
  s8->SetObject(String::AllocFromAscii("robot"));

  Field* s9 = statics->GetStaticField(9);
  EXPECT_EQ('[', s9->GetType());
//  EXPECT_EQ(NULL, s9->GetObject());  // TODO uncomment this
  s9->SetObject(NULL);

  EXPECT_EQ(false,                s0->GetBoolean());
  EXPECT_EQ(6,                    s1->GetByte());
  EXPECT_EQ('b',                  s2->GetChar());
  EXPECT_EQ(65001,                s3->GetShort());
  EXPECT_EQ(2000000001,           s4->GetInt());
  EXPECT_EQ(0x34567890abcdef12LL, s5->GetLong());
  EXPECT_EQ(0.75,                 s6->GetFloat());
  EXPECT_EQ(16777219,             s7->GetDouble());
  EXPECT_TRUE(down_cast<String*>(s8->GetObject())->Equals("robot"));
}

}  // namespace art
