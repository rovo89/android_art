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

    SirtRef<String> string(String::AllocFromModifiedUtf8(length, utf8_in));
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
  EXPECT_TRUE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/lang/Class;"));
  EXPECT_TRUE(Class::IsInSamePackage("LFoo;", "LBar;"));

  // Mismatches
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/io/File;"));
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/lang/reflect/Method;"));
}

TEST_F(ObjectTest, Clone) {
  SirtRef<ObjectArray<Object> > a1(class_linker_->AllocObjectArray<Object>(256));
  size_t s1 = a1->SizeOf();
  Object* clone = a1->Clone();
  EXPECT_EQ(s1, clone->SizeOf());
  EXPECT_TRUE(clone->GetClass() == a1->GetClass());
}

TEST_F(ObjectTest, AllocObjectArray) {
  SirtRef<ObjectArray<Object> > oa(class_linker_->AllocObjectArray<Object>(2));
  EXPECT_EQ(2, oa->GetLength());
  EXPECT_TRUE(oa->Get(0) == NULL);
  EXPECT_TRUE(oa->Get(1) == NULL);
  oa->Set(0, oa.get());
  EXPECT_TRUE(oa->Get(0) == oa.get());
  EXPECT_TRUE(oa->Get(1) == NULL);
  oa->Set(1, oa.get());
  EXPECT_TRUE(oa->Get(0) == oa.get());
  EXPECT_TRUE(oa->Get(1) == oa.get());

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
  ClassHelper oa_ch(oa->GetClass());
  ASSERT_EQ(2U, oa_ch.NumInterfaces());
  EXPECT_EQ(class_linker_->FindSystemClass("Ljava/lang/Cloneable;"), oa_ch.GetInterface(0));
  EXPECT_EQ(class_linker_->FindSystemClass("Ljava/io/Serializable;"), oa_ch.GetInterface(1));
}

TEST_F(ObjectTest, AllocArray) {
  Class* c = class_linker_->FindSystemClass("[I");
  SirtRef<Array> a(Array::Alloc(c, 1));
  ASSERT_TRUE(c == a->GetClass());

  c = class_linker_->FindSystemClass("[Ljava/lang/Object;");
  a.reset(Array::Alloc(c, 1));
  ASSERT_TRUE(c == a->GetClass());

  c = class_linker_->FindSystemClass("[[Ljava/lang/Object;");
  a.reset(Array::Alloc(c, 1));
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
  const DexFile::StringId* string_id = java_lang_dex_file_->FindStringId("[I");
  ASSERT_TRUE(string_id != NULL);
  const DexFile::TypeId* type_id = java_lang_dex_file_->FindTypeId(
      java_lang_dex_file_->GetIndexForStringId(*string_id));
  ASSERT_TRUE(type_id != NULL);
  uint32_t type_idx = java_lang_dex_file_->GetIndexForTypeId(*type_id);
  Object* array = CheckAndAllocArrayFromCode(type_idx, sort, 3, Thread::Current());
  EXPECT_TRUE(array->IsArrayInstance());
  EXPECT_EQ(3, array->AsArray()->GetLength());
  EXPECT_TRUE(array->GetClass()->IsArrayClass());
  EXPECT_TRUE(array->GetClass()->GetComponentType()->IsPrimitive());
}

TEST_F(ObjectTest, StaticFieldFromCode) {
  // pretend we are trying to access 'Static.s0' from StaticsFromCode.<clinit>
  SirtRef<ClassLoader> class_loader(LoadDex("StaticsFromCode"));
  const DexFile* dex_file = ClassLoader::GetCompileTimeClassPath(class_loader.get())[0];
  CHECK(dex_file != NULL);

  Class* klass = class_linker_->FindClass("LStaticsFromCode;", class_loader.get());
  Method* clinit = klass->FindDirectMethod("<clinit>", "()V");
  const DexFile::StringId* klass_string_id = dex_file->FindStringId("LStaticsFromCode;");
  ASSERT_TRUE(klass_string_id != NULL);
  const DexFile::TypeId* klass_type_id = dex_file->FindTypeId(
      dex_file->GetIndexForStringId(*klass_string_id));
  ASSERT_TRUE(klass_type_id != NULL);

  const DexFile::StringId* type_string_id = dex_file->FindStringId("Ljava/lang/Object;");
  ASSERT_TRUE(type_string_id != NULL);
  const DexFile::TypeId* type_type_id = dex_file->FindTypeId(
      dex_file->GetIndexForStringId(*type_string_id));
  ASSERT_TRUE(type_type_id != NULL);

  const DexFile::StringId* name_str_id = dex_file->FindStringId("s0");
  ASSERT_TRUE(name_str_id != NULL);

  const DexFile::FieldId* field_id = dex_file->FindFieldId(
      *klass_type_id, *name_str_id, *type_type_id);
  ASSERT_TRUE(field_id != NULL);
  uint32_t field_idx = dex_file->GetIndexForFieldId(*field_id);

  Field* field = FindFieldFromCode(field_idx, clinit, true);
  Object* s0 = field->GetObj(NULL);
  EXPECT_EQ(NULL, s0);

  SirtRef<CharArray> char_array(CharArray::Alloc(0));
  field->SetObj(NULL, char_array.get());
  EXPECT_EQ(char_array.get(), field->GetObj(NULL));

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
  SirtRef<String> string(String::AllocFromModifiedUtf8("android"));
  EXPECT_TRUE(string->Equals("android"));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  SirtRef<String> empty(String::AllocFromModifiedUtf8(""));
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, StringEquals) {
  SirtRef<String> string(String::AllocFromModifiedUtf8("android"));
  SirtRef<String> string_2(String::AllocFromModifiedUtf8("android"));
  EXPECT_TRUE(string->Equals(string_2.get()));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  SirtRef<String> empty(String::AllocFromModifiedUtf8(""));
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, DescriptorCompare) {
  ClassLinker* linker = class_linker_;

  SirtRef<ClassLoader> class_loader_1(LoadDex("ProtoCompare"));
  SirtRef<ClassLoader> class_loader_2(LoadDex("ProtoCompare2"));

  Class* klass1 = linker->FindClass("LProtoCompare;", class_loader_1.get());
  ASSERT_TRUE(klass1 != NULL);
  Class* klass2 = linker->FindClass("LProtoCompare2;", class_loader_2.get());
  ASSERT_TRUE(klass2 != NULL);

  Method* m1_1 = klass1->GetVirtualMethod(0);
  MethodHelper mh(m1_1);
  EXPECT_STREQ(mh.GetName(), "m1");
  Method* m2_1 = klass1->GetVirtualMethod(1);
  mh.ChangeMethod(m2_1);
  EXPECT_STREQ(mh.GetName(), "m2");
  Method* m3_1 = klass1->GetVirtualMethod(2);
  mh.ChangeMethod(m3_1);
  EXPECT_STREQ(mh.GetName(), "m3");
  Method* m4_1 = klass1->GetVirtualMethod(3);
  mh.ChangeMethod(m4_1);
  EXPECT_STREQ(mh.GetName(), "m4");

  Method* m1_2 = klass2->GetVirtualMethod(0);
  mh.ChangeMethod(m1_2);
  EXPECT_STREQ(mh.GetName(), "m1");
  Method* m2_2 = klass2->GetVirtualMethod(1);
  mh.ChangeMethod(m2_2);
  EXPECT_STREQ(mh.GetName(), "m2");
  Method* m3_2 = klass2->GetVirtualMethod(2);
  mh.ChangeMethod(m3_2);
  EXPECT_STREQ(mh.GetName(), "m3");
  Method* m4_2 = klass2->GetVirtualMethod(3);
  mh.ChangeMethod(m4_2);
  EXPECT_STREQ(mh.GetName(), "m4");

  mh.ChangeMethod(m1_1);
  MethodHelper mh2(m1_2);
  EXPECT_TRUE(mh.HasSameNameAndSignature(&mh2));
  EXPECT_TRUE(mh2.HasSameNameAndSignature(&mh));

  mh.ChangeMethod(m2_1);
  mh2.ChangeMethod(m2_2);
  EXPECT_TRUE(mh.HasSameNameAndSignature(&mh2));
  EXPECT_TRUE(mh2.HasSameNameAndSignature(&mh));

  mh.ChangeMethod(m3_1);
  mh2.ChangeMethod(m3_2);
  EXPECT_TRUE(mh.HasSameNameAndSignature(&mh2));
  EXPECT_TRUE(mh2.HasSameNameAndSignature(&mh));

  mh.ChangeMethod(m4_1);
  mh2.ChangeMethod(m4_2);
  EXPECT_TRUE(mh.HasSameNameAndSignature(&mh2));
  EXPECT_TRUE(mh2.HasSameNameAndSignature(&mh));
}


TEST_F(ObjectTest, StringHashCode) {
  SirtRef<String> empty(String::AllocFromModifiedUtf8(""));
  SirtRef<String> A(String::AllocFromModifiedUtf8("A"));
  SirtRef<String> ABC(String::AllocFromModifiedUtf8("ABC"));

  EXPECT_EQ(0, empty->GetHashCode());
  EXPECT_EQ(65, A->GetHashCode());
  EXPECT_EQ(64578, ABC->GetHashCode());
}

TEST_F(ObjectTest, InstanceOf) {
  SirtRef<ClassLoader> class_loader(LoadDex("XandY"));
  Class* X = class_linker_->FindClass("LX;", class_loader.get());
  Class* Y = class_linker_->FindClass("LY;", class_loader.get());
  ASSERT_TRUE(X != NULL);
  ASSERT_TRUE(Y != NULL);

  SirtRef<Object> x(X->AllocObject());
  SirtRef<Object> y(Y->AllocObject());
  ASSERT_TRUE(x.get() != NULL);
  ASSERT_TRUE(y.get() != NULL);

  EXPECT_EQ(1U, IsAssignableFromCode(X, x->GetClass()));
  EXPECT_EQ(0U, IsAssignableFromCode(Y, x->GetClass()));
  EXPECT_EQ(1U, IsAssignableFromCode(X, y->GetClass()));
  EXPECT_EQ(1U, IsAssignableFromCode(Y, y->GetClass()));

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
  SirtRef<ClassLoader> class_loader(LoadDex("XandY"));
  Class* X = class_linker_->FindClass("LX;", class_loader.get());
  Class* Y = class_linker_->FindClass("LY;", class_loader.get());

  EXPECT_TRUE(X->IsAssignableFrom(X));
  EXPECT_TRUE(X->IsAssignableFrom(Y));
  EXPECT_FALSE(Y->IsAssignableFrom(X));
  EXPECT_TRUE(Y->IsAssignableFrom(Y));

  // class final String implements CharSequence, ..
  Class* string = class_linker_->FindSystemClass("Ljava/lang/String;");
  Class* charseq = class_linker_->FindSystemClass("Ljava/lang/CharSequence;");
  // Can String be assigned to CharSequence without a cast?
  EXPECT_TRUE(charseq->IsAssignableFrom(string));
  // Can CharSequence be assigned to String without a cast?
  EXPECT_FALSE(string->IsAssignableFrom(charseq));

  // Primitive types are only assignable to themselves
  const char* prims = "ZBCSIJFD";
  Class* prim_types[strlen(prims)];
  for (size_t i = 0; i < strlen(prims); i++) {
    prim_types[i] = class_linker_->FindPrimitiveClass(prims[i]);
  }
  for (size_t i = 0; i < strlen(prims); i++) {
    for (size_t j = 0; i < strlen(prims); i++) {
      if (i == j) {
        EXPECT_TRUE(prim_types[i]->IsAssignableFrom(prim_types[j]));
      } else {
        EXPECT_FALSE(prim_types[i]->IsAssignableFrom(prim_types[j]));
      }
    }
  }
}

TEST_F(ObjectTest, IsAssignableFromArray) {
  SirtRef<ClassLoader> class_loader(LoadDex("XandY"));
  Class* X = class_linker_->FindClass("LX;", class_loader.get());
  Class* Y = class_linker_->FindClass("LY;", class_loader.get());
  ASSERT_TRUE(X != NULL);
  ASSERT_TRUE(Y != NULL);

  Class* YA = class_linker_->FindClass("[LY;", class_loader.get());
  Class* YAA = class_linker_->FindClass("[[LY;", class_loader.get());
  ASSERT_TRUE(YA != NULL);
  ASSERT_TRUE(YAA != NULL);

  Class* XAA = class_linker_->FindClass("[[LX;", class_loader.get());
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
  SirtRef<String> s(String::AllocFromModifiedUtf8("ABC"));
  ASSERT_TRUE(s.get() != NULL);
  Class* c = s->GetClass();
  ASSERT_TRUE(c != NULL);

  // Wrong type.
  EXPECT_TRUE(c->FindDeclaredInstanceField("count", "J") == NULL);
  EXPECT_TRUE(c->FindInstanceField("count", "J") == NULL);

  // Wrong name.
  EXPECT_TRUE(c->FindDeclaredInstanceField("Count", "I") == NULL);
  EXPECT_TRUE(c->FindInstanceField("Count", "I") == NULL);

  // Right name and type.
  Field* f1 = c->FindDeclaredInstanceField("count", "I");
  Field* f2 = c->FindInstanceField("count", "I");
  EXPECT_TRUE(f1 != NULL);
  EXPECT_TRUE(f2 != NULL);
  EXPECT_EQ(f1, f2);

  // TODO: check that s.count == 3.

  // Ensure that we handle superclass fields correctly...
  c = class_linker_->FindSystemClass("Ljava/lang/StringBuilder;");
  ASSERT_TRUE(c != NULL);
  // No StringBuilder.count...
  EXPECT_TRUE(c->FindDeclaredInstanceField("count", "I") == NULL);
  // ...but there is an AbstractStringBuilder.count.
  EXPECT_TRUE(c->FindInstanceField("count", "I") != NULL);
}

TEST_F(ObjectTest, FindStaticField) {
  SirtRef<String> s(String::AllocFromModifiedUtf8("ABC"));
  ASSERT_TRUE(s.get() != NULL);
  Class* c = s->GetClass();
  ASSERT_TRUE(c != NULL);

  // Wrong type.
  EXPECT_TRUE(c->FindDeclaredStaticField("CASE_INSENSITIVE_ORDER", "I") == NULL);
  EXPECT_TRUE(c->FindStaticField("CASE_INSENSITIVE_ORDER", "I") == NULL);

  // Wrong name.
  EXPECT_TRUE(c->FindDeclaredStaticField("cASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;") == NULL);
  EXPECT_TRUE(c->FindStaticField("cASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;") == NULL);

  // Right name and type.
  Field* f1 = c->FindDeclaredStaticField("CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  Field* f2 = c->FindStaticField("CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_TRUE(f1 != NULL);
  EXPECT_TRUE(f2 != NULL);
  EXPECT_EQ(f1, f2);

  // TODO: test static fields via superclasses.
  // TODO: test static fields via interfaces.
  // TODO: test that interfaces trump superclasses.
}

}  // namespace art
