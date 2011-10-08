// Copyright 2011 Google Inc. All Rights Reserved.

#include "object.h"

#include <stdint.h>
#include <stdio.h>

#include "UniquePtr.h"
#include "class_linker.h"
#include "common_test.h"
#include "dex_file.h"
#include "heap.h"
#include "runtime_support.h"

namespace art {

class ObjectTest : public CommonTest {
 protected:
  void AssertString(int32_t length,
                    const char* utf8_in,
                    const char* utf16_expected_le,
                    int32_t expected_hash) {
    uint16_t utf16_expected[length];
    for (int32_t i = 0; i < length; i++) {
      uint16_t ch = (((utf16_expected_le[i*2 + 0] & 0xff) << 8) |
                     ((utf16_expected_le[i*2 + 1] & 0xff) << 0));
      utf16_expected[i] = ch;
    }

    String* string = String::AllocFromModifiedUtf8(length, utf8_in);
    ASSERT_EQ(length, string->GetLength());
    ASSERT_TRUE(string->GetCharArray() != NULL);
    ASSERT_TRUE(string->GetCharArray()->GetData() != NULL);
    // strlen is necessary because the 1-character string "\0" is interpreted as ""
    ASSERT_TRUE(string->Equals(utf8_in) || length != static_cast<int32_t>(strlen(utf8_in)));
    for (int32_t i = 0; i < length; i++) {
      EXPECT_EQ(utf16_expected[i], string->CharAt(i));
    }
    EXPECT_EQ(expected_hash, string->GetHashCode());
  }
};

TEST_F(ObjectTest, IsInSamePackage) {
  // Matches
  EXPECT_TRUE(Class::IsInSamePackage(String::AllocFromModifiedUtf8("Ljava/lang/Object;"),
                                     String::AllocFromModifiedUtf8("Ljava/lang/Class")));
  EXPECT_TRUE(Class::IsInSamePackage(String::AllocFromModifiedUtf8("LFoo;"),
                                     String::AllocFromModifiedUtf8("LBar;")));

  // Mismatches
  EXPECT_FALSE(Class::IsInSamePackage(String::AllocFromModifiedUtf8("Ljava/lang/Object;"),
                                      String::AllocFromModifiedUtf8("Ljava/io/File;")));
  EXPECT_FALSE(Class::IsInSamePackage(String::AllocFromModifiedUtf8("Ljava/lang/Object;"),
                                      String::AllocFromModifiedUtf8("Ljava/lang/reflect/Method;")));
}

TEST_F(ObjectTest, Clone) {
  ObjectArray<Object>* a1 = class_linker_->AllocObjectArray<Object>(256);
  size_t s1 = a1->SizeOf();
  Object* clone = a1->Clone();
  EXPECT_EQ(s1, clone->SizeOf());
  EXPECT_TRUE(clone->GetClass() == a1->GetClass());
}

TEST_F(ObjectTest, AllocObjectArray) {
  ObjectArray<Object>* oa = class_linker_->AllocObjectArray<Object>(2);
  EXPECT_EQ(2, oa->GetLength());
  EXPECT_TRUE(oa->Get(0) == NULL);
  EXPECT_TRUE(oa->Get(1) == NULL);
  oa->Set(0, oa);
  EXPECT_TRUE(oa->Get(0) == oa);
  EXPECT_TRUE(oa->Get(1) == NULL);
  oa->Set(1, oa);
  EXPECT_TRUE(oa->Get(0) == oa);
  EXPECT_TRUE(oa->Get(1) == oa);

  Thread* self = Thread::Current();
  Class* aioobe = class_linker_->FindSystemClass("Ljava/lang/ArrayIndexOutOfBoundsException;");

  EXPECT_TRUE(oa->Get(-1) == NULL);
  EXPECT_TRUE(self->IsExceptionPending());
  EXPECT_EQ(aioobe, self->GetException()->GetClass());
  self->ClearException();

  EXPECT_TRUE(oa->Get(2) == NULL);
  EXPECT_TRUE(self->IsExceptionPending());
  EXPECT_EQ(aioobe, self->GetException()->GetClass());
  self->ClearException();

  ASSERT_TRUE(oa->GetClass() != NULL);
  ASSERT_EQ(2U, oa->GetClass()->NumInterfaces());
  EXPECT_EQ(class_linker_->FindSystemClass("Ljava/lang/Cloneable;"),
            oa->GetClass()->GetInterface(0));
  EXPECT_EQ(class_linker_->FindSystemClass("Ljava/io/Serializable;"),
            oa->GetClass()->GetInterface(1));
}

TEST_F(ObjectTest, AllocArray) {
  Class* c = class_linker_->FindSystemClass("[I");
  Array* a = Array::Alloc(c, 1);
  ASSERT_TRUE(c == a->GetClass());

  c = class_linker_->FindSystemClass("[Ljava/lang/Object;");
  a = Array::Alloc(c, 1);
  ASSERT_TRUE(c == a->GetClass());

  c = class_linker_->FindSystemClass("[[Ljava/lang/Object;");
  a = Array::Alloc(c, 1);
  ASSERT_TRUE(c == a->GetClass());
}

template<typename ArrayT>
void TestPrimitiveArray(ClassLinker* cl) {
  typedef typename ArrayT::ElementType T;

  ArrayT* a = ArrayT::Alloc(2);
  EXPECT_EQ(2, a->GetLength());
  EXPECT_EQ(0, a->Get(0));
  EXPECT_EQ(0, a->Get(1));
  a->Set(0, T(123));
  EXPECT_EQ(T(123), a->Get(0));
  EXPECT_EQ(0, a->Get(1));
  a->Set(1, T(321));
  EXPECT_EQ(T(123), a->Get(0));
  EXPECT_EQ(T(321), a->Get(1));

  Thread* self = Thread::Current();
  Class* aioobe = cl->FindSystemClass("Ljava/lang/ArrayIndexOutOfBoundsException;");

  EXPECT_EQ(0, a->Get(-1));
  EXPECT_TRUE(self->IsExceptionPending());
  EXPECT_EQ(aioobe, self->GetException()->GetClass());
  self->ClearException();

  EXPECT_EQ(0, a->Get(2));
  EXPECT_TRUE(self->IsExceptionPending());
  EXPECT_EQ(aioobe, self->GetException()->GetClass());
  self->ClearException();
}

TEST_F(ObjectTest, PrimitiveArray_Boolean_Alloc) {
  TestPrimitiveArray<BooleanArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Byte_Alloc) {
  TestPrimitiveArray<ByteArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Char_Alloc) {
  TestPrimitiveArray<CharArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Double_Alloc) {
  TestPrimitiveArray<DoubleArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Float_Alloc) {
  TestPrimitiveArray<FloatArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Int_Alloc) {
  TestPrimitiveArray<IntArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Long_Alloc) {
  TestPrimitiveArray<LongArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Short_Alloc) {
  TestPrimitiveArray<ShortArray>(class_linker_);
}

TEST_F(ObjectTest, CheckAndAllocArrayFromCode) {
  // pretend we are trying to call 'new char[3]' from String.toCharArray
  Class* java_util_Arrays = class_linker_->FindSystemClass("Ljava/util/Arrays;");
  Method* sort = java_util_Arrays->FindDirectMethod("sort", "([I)V");
  uint32_t type_idx = FindTypeIdxByDescriptor(*java_lang_dex_file_.get(), "[I");
  Object* array = CheckAndAllocArrayFromCode(type_idx, sort, 3, Thread::Current());
  EXPECT_TRUE(array->IsArrayInstance());
  EXPECT_EQ(3, array->AsArray()->GetLength());
  EXPECT_TRUE(array->GetClass()->IsArrayClass());
  EXPECT_TRUE(array->GetClass()->GetComponentType()->IsPrimitive());
}

TEST_F(ObjectTest, StaticFieldFromCode) {
  // pretend we are trying to access 'Static.s0' from StaticsFromCode.<clinit>
  const ClassLoader* class_loader = LoadDex("StaticsFromCode");
  const DexFile* dex_file = ClassLoader::GetClassPath(class_loader)[0];
  CHECK(dex_file != NULL);

  Class* klass = class_linker_->FindClass("LStaticsFromCode;", class_loader);
  Method* clinit = klass->FindDirectMethod("<clinit>", "()V");
  uint32_t field_idx = FindFieldIdxByDescriptorAndName(*dex_file, "LStaticsFromCode;", "s0");
  Field* field = FindFieldFromCode(field_idx, clinit, true);
  Object* s0 = field->GetObj(NULL);
  EXPECT_EQ(NULL, s0);

  CharArray* char_array = CharArray::Alloc(0);
  field->SetObj(NULL, char_array);
  EXPECT_EQ(char_array, field->GetObj(NULL));

  field->SetObj(NULL, NULL);
  EXPECT_EQ(NULL, field->GetObj(NULL));

  // TODO: more exhaustive tests of all 6 cases of Field::*FromCode
}

TEST_F(ObjectTest, String) {
  // Test the empty string.
  AssertString(0, "",     "", 0);

  // Test one-byte characters.
  AssertString(1, " ",    "\x00\x20",         0x20);
  AssertString(1, "",     "\x00\x00",         0);
  AssertString(1, "\x7f", "\x00\x7f",         0x7f);
  AssertString(2, "hi",   "\x00\x68\x00\x69", (31 * 0x68) + 0x69);

  // Test two-byte characters.
  AssertString(1, "\xc2\x80",   "\x00\x80",                 0x80);
  AssertString(1, "\xd9\xa6",   "\x06\x66",                 0x0666);
  AssertString(1, "\xdf\xbf",   "\x07\xff",                 0x07ff);
  AssertString(3, "h\xd9\xa6i", "\x00\x68\x06\x66\x00\x69", (31 * ((31 * 0x68) + 0x0666)) + 0x69);

  // Test three-byte characters.
  AssertString(1, "\xe0\xa0\x80",   "\x08\x00",                 0x0800);
  AssertString(1, "\xe1\x88\xb4",   "\x12\x34",                 0x1234);
  AssertString(1, "\xef\xbf\xbf",   "\xff\xff",                 0xffff);
  AssertString(3, "h\xe1\x88\xb4i", "\x00\x68\x12\x34\x00\x69", (31 * ((31 * 0x68) + 0x1234)) + 0x69);
}

TEST_F(ObjectTest, StringEqualsUtf8) {
  String* string = String::AllocFromModifiedUtf8("android");
  EXPECT_TRUE(string->Equals("android"));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  String* empty = String::AllocFromModifiedUtf8("");
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, StringEquals) {
  String* string = String::AllocFromModifiedUtf8("android");
  EXPECT_TRUE(string->Equals(String::AllocFromModifiedUtf8("android")));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  String* empty = String::AllocFromModifiedUtf8("");
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, DescriptorCompare) {
  ClassLinker* linker = class_linker_;

  const ClassLoader* class_loader_1 = LoadDex("ProtoCompare");
  const ClassLoader* class_loader_2 = LoadDex("ProtoCompare2");

  Class* klass1 = linker->FindClass("LProtoCompare;", class_loader_1);
  ASSERT_TRUE(klass1 != NULL);
  Class* klass2 = linker->FindClass("LProtoCompare2;", class_loader_2);
  ASSERT_TRUE(klass2 != NULL);

  Method* m1_1 = klass1->GetVirtualMethod(0);
  EXPECT_TRUE(m1_1->GetName()->Equals("m1"));
  Method* m2_1 = klass1->GetVirtualMethod(1);
  EXPECT_TRUE(m2_1->GetName()->Equals("m2"));
  Method* m3_1 = klass1->GetVirtualMethod(2);
  EXPECT_TRUE(m3_1->GetName()->Equals("m3"));
  Method* m4_1 = klass1->GetVirtualMethod(3);
  EXPECT_TRUE(m4_1->GetName()->Equals("m4"));

  Method* m1_2 = klass2->GetVirtualMethod(0);
  EXPECT_TRUE(m1_2->GetName()->Equals("m1"));
  Method* m2_2 = klass2->GetVirtualMethod(1);
  EXPECT_TRUE(m2_2->GetName()->Equals("m2"));
  Method* m3_2 = klass2->GetVirtualMethod(2);
  EXPECT_TRUE(m3_2->GetName()->Equals("m3"));
  Method* m4_2 = klass2->GetVirtualMethod(3);
  EXPECT_TRUE(m4_2->GetName()->Equals("m4"));

  EXPECT_TRUE(m1_1->HasSameNameAndDescriptor(m1_2));
  EXPECT_TRUE(m1_2->HasSameNameAndDescriptor(m1_1));

  EXPECT_TRUE(m2_1->HasSameNameAndDescriptor(m2_2));
  EXPECT_TRUE(m2_2->HasSameNameAndDescriptor(m2_1));

  EXPECT_TRUE(m3_1->HasSameNameAndDescriptor(m3_2));
  EXPECT_TRUE(m3_2->HasSameNameAndDescriptor(m3_1));

  EXPECT_TRUE(m4_1->HasSameNameAndDescriptor(m4_2));
  EXPECT_TRUE(m4_2->HasSameNameAndDescriptor(m4_1));
}


TEST_F(ObjectTest, StringHashCode) {
  EXPECT_EQ(0, String::AllocFromModifiedUtf8("")->GetHashCode());
  EXPECT_EQ(65, String::AllocFromModifiedUtf8("A")->GetHashCode());
  EXPECT_EQ(64578, String::AllocFromModifiedUtf8("ABC")->GetHashCode());
}

TEST_F(ObjectTest, InstanceOf) {
  const ClassLoader* class_loader = LoadDex("XandY");
  Class* X = class_linker_->FindClass("LX;", class_loader);
  Class* Y = class_linker_->FindClass("LY;", class_loader);
  ASSERT_TRUE(X != NULL);
  ASSERT_TRUE(Y != NULL);

  Object* x = X->AllocObject();
  Object* y = Y->AllocObject();
  ASSERT_TRUE(x != NULL);
  ASSERT_TRUE(y != NULL);

  EXPECT_EQ(1U, Class::IsAssignableFromCode(X, x->GetClass()));
  EXPECT_EQ(0U, Class::IsAssignableFromCode(Y, x->GetClass()));
  EXPECT_EQ(1U, Class::IsAssignableFromCode(X, y->GetClass()));
  EXPECT_EQ(1U, Class::IsAssignableFromCode(Y, y->GetClass()));

  EXPECT_TRUE(x->InstanceOf(X));
  EXPECT_FALSE(x->InstanceOf(Y));
  EXPECT_TRUE(y->InstanceOf(X));
  EXPECT_TRUE(y->InstanceOf(Y));

  Class* java_lang_Class = class_linker_->FindSystemClass("Ljava/lang/Class;");
  Class* Object_array_class = class_linker_->FindSystemClass("[Ljava/lang/Object;");

  EXPECT_FALSE(java_lang_Class->InstanceOf(Object_array_class));
  EXPECT_TRUE(Object_array_class->InstanceOf(java_lang_Class));

  // All array classes implement Cloneable and Serializable.
  Object* array = ObjectArray<Object>::Alloc(Object_array_class, 1);
  Class* java_lang_Cloneable = class_linker_->FindSystemClass("Ljava/lang/Cloneable;");
  Class* java_io_Serializable = class_linker_->FindSystemClass("Ljava/io/Serializable;");
  EXPECT_TRUE(array->InstanceOf(java_lang_Cloneable));
  EXPECT_TRUE(array->InstanceOf(java_io_Serializable));
}

TEST_F(ObjectTest, IsAssignableFrom) {
  const ClassLoader* class_loader = LoadDex("XandY");
  Class* X = class_linker_->FindClass("LX;", class_loader);
  Class* Y = class_linker_->FindClass("LY;", class_loader);

  EXPECT_TRUE(X->IsAssignableFrom(X));
  EXPECT_TRUE(X->IsAssignableFrom(Y));
  EXPECT_FALSE(Y->IsAssignableFrom(X));
  EXPECT_TRUE(Y->IsAssignableFrom(Y));
}

TEST_F(ObjectTest, IsAssignableFromArray) {
  const ClassLoader* class_loader = LoadDex("XandY");
  Class* X = class_linker_->FindClass("LX;", class_loader);
  Class* Y = class_linker_->FindClass("LY;", class_loader);
  ASSERT_TRUE(X != NULL);
  ASSERT_TRUE(Y != NULL);

  Class* YA = class_linker_->FindClass("[LY;", class_loader);
  Class* YAA = class_linker_->FindClass("[[LY;", class_loader);
  ASSERT_TRUE(YA != NULL);
  ASSERT_TRUE(YAA != NULL);

  Class* XAA = class_linker_->FindClass("[[LX;", class_loader);
  ASSERT_TRUE(XAA != NULL);

  Class* O = class_linker_->FindSystemClass("Ljava/lang/Object;");
  Class* OA = class_linker_->FindSystemClass("[Ljava/lang/Object;");
  Class* OAA = class_linker_->FindSystemClass("[[Ljava/lang/Object;");
  Class* OAAA = class_linker_->FindSystemClass("[[[Ljava/lang/Object;");
  ASSERT_TRUE(O != NULL);
  ASSERT_TRUE(OA != NULL);
  ASSERT_TRUE(OAA != NULL);
  ASSERT_TRUE(OAAA != NULL);

  Class* S = class_linker_->FindSystemClass("Ljava/io/Serializable;");
  Class* SA = class_linker_->FindSystemClass("[Ljava/io/Serializable;");
  Class* SAA = class_linker_->FindSystemClass("[[Ljava/io/Serializable;");
  ASSERT_TRUE(S != NULL);
  ASSERT_TRUE(SA != NULL);
  ASSERT_TRUE(SAA != NULL);

  Class* IA = class_linker_->FindSystemClass("[I");
  ASSERT_TRUE(IA != NULL);

  EXPECT_TRUE(YAA->IsAssignableFrom(YAA));  // identity
  EXPECT_TRUE(XAA->IsAssignableFrom(YAA));  // element superclass
  EXPECT_FALSE(YAA->IsAssignableFrom(XAA));
  EXPECT_FALSE(Y->IsAssignableFrom(YAA));
  EXPECT_FALSE(YA->IsAssignableFrom(YAA));
  EXPECT_TRUE(O->IsAssignableFrom(YAA));  // everything is an Object
  EXPECT_TRUE(OA->IsAssignableFrom(YAA));
  EXPECT_TRUE(OAA->IsAssignableFrom(YAA));
  EXPECT_TRUE(S->IsAssignableFrom(YAA));  // all arrays are Serializable
  EXPECT_TRUE(SA->IsAssignableFrom(YAA));
  EXPECT_FALSE(SAA->IsAssignableFrom(YAA));  // unless Y was Serializable

  EXPECT_FALSE(IA->IsAssignableFrom(OA));
  EXPECT_FALSE(OA->IsAssignableFrom(IA));
  EXPECT_TRUE(O->IsAssignableFrom(IA));
}

TEST_F(ObjectTest, FindInstanceField) {
  String* s = String::AllocFromModifiedUtf8("ABC");
  ASSERT_TRUE(s != NULL);
  Class* c = s->GetClass();
  ASSERT_TRUE(c != NULL);

  // Wrong type.
  EXPECT_TRUE(c->FindDeclaredInstanceField("count", class_linker_->FindSystemClass("J")) == NULL);
  EXPECT_TRUE(c->FindInstanceField("count", class_linker_->FindSystemClass("J")) == NULL);

  // Wrong name.
  EXPECT_TRUE(c->FindDeclaredInstanceField("Count", class_linker_->FindSystemClass("I")) == NULL);
  EXPECT_TRUE(c->FindInstanceField("Count", class_linker_->FindSystemClass("I")) == NULL);

  // Right name and type.
  Field* f1 = c->FindDeclaredInstanceField("count", class_linker_->FindSystemClass("I"));
  Field* f2 = c->FindInstanceField("count", class_linker_->FindSystemClass("I"));
  EXPECT_TRUE(f1 != NULL);
  EXPECT_TRUE(f2 != NULL);
  EXPECT_EQ(f1, f2);

  // TODO: check that s.count == 3.

  // Ensure that we handle superclass fields correctly...
  c = class_linker_->FindSystemClass("Ljava/lang/StringBuilder;");
  ASSERT_TRUE(c != NULL);
  // No StringBuilder.count...
  EXPECT_TRUE(c->FindDeclaredInstanceField("count", class_linker_->FindSystemClass("I")) == NULL);
  // ...but there is an AbstractStringBuilder.count.
  EXPECT_TRUE(c->FindInstanceField("count", class_linker_->FindSystemClass("I")) != NULL);
}

TEST_F(ObjectTest, FindStaticField) {
  String* s = String::AllocFromModifiedUtf8("ABC");
  ASSERT_TRUE(s != NULL);
  Class* c = s->GetClass();
  ASSERT_TRUE(c != NULL);

  // Wrong type.
  EXPECT_TRUE(c->FindDeclaredStaticField("CASE_INSENSITIVE_ORDER", class_linker_->FindSystemClass("I")) == NULL);
  EXPECT_TRUE(c->FindStaticField("CASE_INSENSITIVE_ORDER", class_linker_->FindSystemClass("I")) == NULL);

  // Wrong name.
  EXPECT_TRUE(c->FindDeclaredStaticField("cASE_INSENSITIVE_ORDER", class_linker_->FindSystemClass("Ljava/util/Comparator;")) == NULL);
  EXPECT_TRUE(c->FindStaticField("cASE_INSENSITIVE_ORDER", class_linker_->FindSystemClass("Ljava/util/Comparator;")) == NULL);

  // Right name and type.
  Field* f1 = c->FindDeclaredStaticField("CASE_INSENSITIVE_ORDER", class_linker_->FindSystemClass("Ljava/util/Comparator;"));
  Field* f2 = c->FindStaticField("CASE_INSENSITIVE_ORDER", class_linker_->FindSystemClass("Ljava/util/Comparator;"));
  EXPECT_TRUE(f1 != NULL);
  EXPECT_TRUE(f2 != NULL);
  EXPECT_EQ(f1, f2);

  // TODO: test static fields via superclasses.
  // TODO: test static fields via interfaces.
  // TODO: test that interfaces trump superclasses.
}

}  // namespace art
