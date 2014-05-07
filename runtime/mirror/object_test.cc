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

#include "object.h"

#include <stdint.h>
#include <stdio.h>

#include "array-inl.h"
#include "art_field-inl.h"
#include "asm_support.h"
#include "class-inl.h"
#include "class_linker.h"
#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "dex_file.h"
#include "entrypoints/entrypoint_utils.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "iftable-inl.h"
#include "art_method-inl.h"
#include "object-inl.h"
#include "object_array-inl.h"
#include "handle_scope-inl.h"
#include "string-inl.h"
#include "UniquePtr.h"

namespace art {
namespace mirror {

class ObjectTest : public CommonRuntimeTest {
 protected:
  void AssertString(int32_t expected_utf16_length,
                    const char* utf8_in,
                    const char* utf16_expected_le,
                    int32_t expected_hash)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    UniquePtr<uint16_t[]> utf16_expected(new uint16_t[expected_utf16_length]);
    for (int32_t i = 0; i < expected_utf16_length; i++) {
      uint16_t ch = (((utf16_expected_le[i*2 + 0] & 0xff) << 8) |
                     ((utf16_expected_le[i*2 + 1] & 0xff) << 0));
      utf16_expected[i] = ch;
    }

    Thread* self = Thread::Current();
    StackHandleScope<1> hs(self);
    Handle<String> string(
        hs.NewHandle(String::AllocFromModifiedUtf8(self, expected_utf16_length, utf8_in)));
    ASSERT_EQ(expected_utf16_length, string->GetLength());
    ASSERT_TRUE(string->GetCharArray() != NULL);
    ASSERT_TRUE(string->GetCharArray()->GetData() != NULL);
    // strlen is necessary because the 1-character string "\x00\x00" is interpreted as ""
    ASSERT_TRUE(string->Equals(utf8_in) || (expected_utf16_length == 1 && strlen(utf8_in) == 0));
    ASSERT_TRUE(string->Equals(StringPiece(utf8_in)) || (expected_utf16_length == 1 && strlen(utf8_in) == 0));
    for (int32_t i = 0; i < expected_utf16_length; i++) {
      EXPECT_EQ(utf16_expected[i], string->CharAt(i));
    }
    EXPECT_EQ(expected_hash, string->GetHashCode());
  }
};

// Keep the assembly code in sync
TEST_F(ObjectTest, AsmConstants) {
  EXPECT_EQ(CLASS_OFFSET, Object::ClassOffset().Int32Value());
  EXPECT_EQ(LOCK_WORD_OFFSET, Object::MonitorOffset().Int32Value());

  EXPECT_EQ(CLASS_COMPONENT_TYPE_OFFSET, Class::ComponentTypeOffset().Int32Value());

  EXPECT_EQ(ARRAY_LENGTH_OFFSET, Array::LengthOffset().Int32Value());
  EXPECT_EQ(OBJECT_ARRAY_DATA_OFFSET, Array::DataOffset(sizeof(HeapReference<Object>)).Int32Value());

  EXPECT_EQ(STRING_VALUE_OFFSET, String::ValueOffset().Int32Value());
  EXPECT_EQ(STRING_COUNT_OFFSET, String::CountOffset().Int32Value());
  EXPECT_EQ(STRING_OFFSET_OFFSET, String::OffsetOffset().Int32Value());
  EXPECT_EQ(STRING_DATA_OFFSET, Array::DataOffset(sizeof(uint16_t)).Int32Value());

  EXPECT_EQ(METHOD_DEX_CACHE_METHODS_OFFSET, ArtMethod::DexCacheResolvedMethodsOffset().Int32Value());
  EXPECT_EQ(METHOD_PORTABLE_CODE_OFFSET, ArtMethod::EntryPointFromPortableCompiledCodeOffset().Int32Value());
  EXPECT_EQ(METHOD_QUICK_CODE_OFFSET, ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value());
}

TEST_F(ObjectTest, IsInSamePackage) {
  // Matches
  EXPECT_TRUE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/lang/Class;"));
  EXPECT_TRUE(Class::IsInSamePackage("LFoo;", "LBar;"));

  // Mismatches
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/io/File;"));
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/lang/reflect/Method;"));
}

TEST_F(ObjectTest, Clone) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<ObjectArray<Object>> a1(
      hs.NewHandle(class_linker_->AllocObjectArray<Object>(soa.Self(), 256)));
  size_t s1 = a1->SizeOf();
  Object* clone = a1->Clone(soa.Self());
  EXPECT_EQ(s1, clone->SizeOf());
  EXPECT_TRUE(clone->GetClass() == a1->GetClass());
}

TEST_F(ObjectTest, AllocObjectArray) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<ObjectArray<Object> > oa(
      hs.NewHandle(class_linker_->AllocObjectArray<Object>(soa.Self(), 2)));
  EXPECT_EQ(2, oa->GetLength());
  EXPECT_TRUE(oa->Get(0) == NULL);
  EXPECT_TRUE(oa->Get(1) == NULL);
  oa->Set<false>(0, oa.Get());
  EXPECT_TRUE(oa->Get(0) == oa.Get());
  EXPECT_TRUE(oa->Get(1) == NULL);
  oa->Set<false>(1, oa.Get());
  EXPECT_TRUE(oa->Get(0) == oa.Get());
  EXPECT_TRUE(oa->Get(1) == oa.Get());

  Class* aioobe = class_linker_->FindSystemClass(soa.Self(),
                                                 "Ljava/lang/ArrayIndexOutOfBoundsException;");

  EXPECT_TRUE(oa->Get(-1) == NULL);
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException(NULL)->GetClass());
  soa.Self()->ClearException();

  EXPECT_TRUE(oa->Get(2) == NULL);
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException(NULL)->GetClass());
  soa.Self()->ClearException();

  ASSERT_TRUE(oa->GetClass() != NULL);
  ClassHelper oa_ch(oa->GetClass());
  ASSERT_EQ(2U, oa_ch.NumDirectInterfaces());
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Cloneable;"),
            oa_ch.GetDirectInterface(0));
  EXPECT_EQ(class_linker_->FindSystemClass(soa.Self(), "Ljava/io/Serializable;"),
            oa_ch.GetDirectInterface(1));
}

TEST_F(ObjectTest, AllocArray) {
  ScopedObjectAccess soa(Thread::Current());
  Class* c = class_linker_->FindSystemClass(soa.Self(), "[I");
  StackHandleScope<1> hs(soa.Self());
  Handle<Array> a(
      hs.NewHandle(Array::Alloc<true>(soa.Self(), c, 1, c->GetComponentSize(),
                                      Runtime::Current()->GetHeap()->GetCurrentAllocator())));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_EQ(1, a->GetLength());

  c = class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;");
  a.Assign(Array::Alloc<true>(soa.Self(), c, 1, c->GetComponentSize(),
                              Runtime::Current()->GetHeap()->GetCurrentAllocator()));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_EQ(1, a->GetLength());

  c = class_linker_->FindSystemClass(soa.Self(), "[[Ljava/lang/Object;");
  a.Assign(Array::Alloc<true>(soa.Self(), c, 1, c->GetComponentSize(),
                              Runtime::Current()->GetHeap()->GetCurrentAllocator()));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_EQ(1, a->GetLength());
}

TEST_F(ObjectTest, AllocArray_FillUsable) {
  ScopedObjectAccess soa(Thread::Current());
  Class* c = class_linker_->FindSystemClass(soa.Self(), "[B");
  StackHandleScope<1> hs(soa.Self());
  Handle<Array> a(
      hs.NewHandle(Array::Alloc<true>(soa.Self(), c, 1, c->GetComponentSize(),
                                      Runtime::Current()->GetHeap()->GetCurrentAllocator(), true)));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_LE(1, a->GetLength());

  c = class_linker_->FindSystemClass(soa.Self(), "[I");
  a.Assign(Array::Alloc<true>(soa.Self(), c, 2, c->GetComponentSize(),
                              Runtime::Current()->GetHeap()->GetCurrentAllocator(), true));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_LE(2, a->GetLength());

  c = class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;");
  a.Assign(Array::Alloc<true>(soa.Self(), c, 2, c->GetComponentSize(),
                              Runtime::Current()->GetHeap()->GetCurrentAllocator(), true));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_LE(2, a->GetLength());

  c = class_linker_->FindSystemClass(soa.Self(), "[[Ljava/lang/Object;");
  a.Assign(Array::Alloc<true>(soa.Self(), c, 2, c->GetComponentSize(),
                             Runtime::Current()->GetHeap()->GetCurrentAllocator(), true));
  EXPECT_TRUE(c == a->GetClass());
  EXPECT_LE(2, a->GetLength());
}

template<typename ArrayT>
void TestPrimitiveArray(ClassLinker* cl) {
  ScopedObjectAccess soa(Thread::Current());
  typedef typename ArrayT::ElementType T;

  ArrayT* a = ArrayT::Alloc(soa.Self(), 2);
  EXPECT_EQ(2, a->GetLength());
  EXPECT_EQ(0, a->Get(0));
  EXPECT_EQ(0, a->Get(1));
  a->Set(0, T(123));
  EXPECT_EQ(T(123), a->Get(0));
  EXPECT_EQ(0, a->Get(1));
  a->Set(1, T(321));
  EXPECT_EQ(T(123), a->Get(0));
  EXPECT_EQ(T(321), a->Get(1));

  Class* aioobe = cl->FindSystemClass(soa.Self(), "Ljava/lang/ArrayIndexOutOfBoundsException;");

  EXPECT_EQ(0, a->Get(-1));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException(NULL)->GetClass());
  soa.Self()->ClearException();

  EXPECT_EQ(0, a->Get(2));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(aioobe, soa.Self()->GetException(NULL)->GetClass());
  soa.Self()->ClearException();
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
  ScopedObjectAccess soa(Thread::Current());
  Class* java_util_Arrays = class_linker_->FindSystemClass(soa.Self(), "Ljava/util/Arrays;");
  ArtMethod* sort = java_util_Arrays->FindDirectMethod("sort", "([I)V");
  const DexFile::StringId* string_id = java_lang_dex_file_->FindStringId("[I");
  ASSERT_TRUE(string_id != NULL);
  const DexFile::TypeId* type_id = java_lang_dex_file_->FindTypeId(
      java_lang_dex_file_->GetIndexForStringId(*string_id));
  ASSERT_TRUE(type_id != NULL);
  uint32_t type_idx = java_lang_dex_file_->GetIndexForTypeId(*type_id);
  Object* array = CheckAndAllocArrayFromCodeInstrumented(type_idx, sort, 3, Thread::Current(), false,
                                                         Runtime::Current()->GetHeap()->GetCurrentAllocator());
  EXPECT_TRUE(array->IsArrayInstance());
  EXPECT_EQ(3, array->AsArray()->GetLength());
  EXPECT_TRUE(array->GetClass()->IsArrayClass());
  EXPECT_TRUE(array->GetClass()->GetComponentType()->IsPrimitive());
}

TEST_F(ObjectTest, CreateMultiArray) {
  ScopedObjectAccess soa(Thread::Current());

  StackHandleScope<2> hs(soa.Self());
  Handle<Class> c(hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "I")));
  Handle<IntArray> dims(hs.NewHandle(IntArray::Alloc(soa.Self(), 1)));
  dims->Set<false>(0, 1);
  Array* multi = Array::CreateMultiArray(soa.Self(), c, dims);
  EXPECT_TRUE(multi->GetClass() == class_linker_->FindSystemClass(soa.Self(), "[I"));
  EXPECT_EQ(1, multi->GetLength());

  dims->Set<false>(0, -1);
  multi = Array::CreateMultiArray(soa.Self(), c, dims);
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(PrettyDescriptor(soa.Self()->GetException(NULL)->GetClass()),
            "java.lang.NegativeArraySizeException");
  soa.Self()->ClearException();

  dims.Assign(IntArray::Alloc(soa.Self(), 2));
  for (int i = 1; i < 20; ++i) {
    for (int j = 0; j < 20; ++j) {
      dims->Set<false>(0, i);
      dims->Set<false>(1, j);
      multi = Array::CreateMultiArray(soa.Self(), c, dims);
      EXPECT_TRUE(multi->GetClass() == class_linker_->FindSystemClass(soa.Self(), "[[I"));
      EXPECT_EQ(i, multi->GetLength());
      for (int k = 0; k < i; ++k) {
        Array* outer = multi->AsObjectArray<Array>()->Get(k);
        EXPECT_TRUE(outer->GetClass() == class_linker_->FindSystemClass(soa.Self(), "[I"));
        EXPECT_EQ(j, outer->GetLength());
      }
    }
  }
}

TEST_F(ObjectTest, StaticFieldFromCode) {
  // pretend we are trying to access 'Static.s0' from StaticsFromCode.<clinit>
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("StaticsFromCode");
  const DexFile* dex_file = Runtime::Current()->GetCompileTimeClassPath(class_loader)[0];
  CHECK(dex_file != NULL);

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(hs.NewHandle(soa.Decode<ClassLoader*>(class_loader)));
  Class* klass = class_linker_->FindClass(soa.Self(), "LStaticsFromCode;", loader);
  ArtMethod* clinit = klass->FindClassInitializer();
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

  ArtField* field = FindFieldFromCode<StaticObjectRead, true>(field_idx, clinit, Thread::Current(),
                                                              sizeof(HeapReference<Object>));
  Object* s0 = field->GetObj(klass);
  EXPECT_TRUE(s0 != NULL);

  Handle<CharArray> char_array(hs.NewHandle(CharArray::Alloc(soa.Self(), 0)));
  field->SetObj<false>(field->GetDeclaringClass(), char_array.Get());
  EXPECT_EQ(char_array.Get(), field->GetObj(klass));

  field->SetObj<false>(field->GetDeclaringClass(), NULL);
  EXPECT_EQ(NULL, field->GetObj(klass));

  // TODO: more exhaustive tests of all 6 cases of ArtField::*FromCode
}

TEST_F(ObjectTest, String) {
  ScopedObjectAccess soa(Thread::Current());
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
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  EXPECT_TRUE(string->Equals("android"));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  Handle<String> empty(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, StringEquals) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  Handle<String> string_2(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  EXPECT_TRUE(string->Equals(string_2.Get()));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  Handle<String> empty(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, StringCompareTo) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<5> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  Handle<String> string_2(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  Handle<String> string_3(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "Android")));
  Handle<String> string_4(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "and")));
  Handle<String> string_5(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  EXPECT_EQ(0, string->CompareTo(string_2.Get()));
  EXPECT_LT(0, string->CompareTo(string_3.Get()));
  EXPECT_GT(0, string_3->CompareTo(string.Get()));
  EXPECT_LT(0, string->CompareTo(string_4.Get()));
  EXPECT_GT(0, string_4->CompareTo(string.Get()));
  EXPECT_LT(0, string->CompareTo(string_5.Get()));
  EXPECT_GT(0, string_5->CompareTo(string.Get()));
}

TEST_F(ObjectTest, StringLength) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  EXPECT_EQ(string->GetLength(), 7);
  EXPECT_EQ(string->GetUtfLength(), 7);

  string->SetOffset(2);
  string->SetCount(5);
  EXPECT_TRUE(string->Equals("droid"));
  EXPECT_EQ(string->GetLength(), 5);
  EXPECT_EQ(string->GetUtfLength(), 5);
}

TEST_F(ObjectTest, DescriptorCompare) {
  // Two classloaders conflicts in compile_time_class_paths_.
  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* linker = class_linker_;

  jobject jclass_loader_1 = LoadDex("ProtoCompare");
  jobject jclass_loader_2 = LoadDex("ProtoCompare2");
  StackHandleScope<2> hs(soa.Self());
  Handle<ClassLoader> class_loader_1(hs.NewHandle(soa.Decode<ClassLoader*>(jclass_loader_1)));
  Handle<ClassLoader> class_loader_2(hs.NewHandle(soa.Decode<ClassLoader*>(jclass_loader_2)));

  Class* klass1 = linker->FindClass(soa.Self(), "LProtoCompare;", class_loader_1);
  ASSERT_TRUE(klass1 != NULL);
  Class* klass2 = linker->FindClass(soa.Self(), "LProtoCompare2;", class_loader_2);
  ASSERT_TRUE(klass2 != NULL);

  ArtMethod* m1_1 = klass1->GetVirtualMethod(0);
  MethodHelper mh(m1_1);
  EXPECT_STREQ(mh.GetName(), "m1");
  ArtMethod* m2_1 = klass1->GetVirtualMethod(1);
  mh.ChangeMethod(m2_1);
  EXPECT_STREQ(mh.GetName(), "m2");
  ArtMethod* m3_1 = klass1->GetVirtualMethod(2);
  mh.ChangeMethod(m3_1);
  EXPECT_STREQ(mh.GetName(), "m3");
  ArtMethod* m4_1 = klass1->GetVirtualMethod(3);
  mh.ChangeMethod(m4_1);
  EXPECT_STREQ(mh.GetName(), "m4");

  ArtMethod* m1_2 = klass2->GetVirtualMethod(0);
  mh.ChangeMethod(m1_2);
  EXPECT_STREQ(mh.GetName(), "m1");
  ArtMethod* m2_2 = klass2->GetVirtualMethod(1);
  mh.ChangeMethod(m2_2);
  EXPECT_STREQ(mh.GetName(), "m2");
  ArtMethod* m3_2 = klass2->GetVirtualMethod(2);
  mh.ChangeMethod(m3_2);
  EXPECT_STREQ(mh.GetName(), "m3");
  ArtMethod* m4_2 = klass2->GetVirtualMethod(3);
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
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<String> empty(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  Handle<String> A(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "A")));
  Handle<String> ABC(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "ABC")));

  EXPECT_EQ(0, empty->GetHashCode());
  EXPECT_EQ(65, A->GetHashCode());
  EXPECT_EQ(64578, ABC->GetHashCode());
}

TEST_F(ObjectTest, InstanceOf) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("XandY");
  StackHandleScope<3> hs(soa.Self());
  Handle<ClassLoader> class_loader(hs.NewHandle(soa.Decode<ClassLoader*>(jclass_loader)));

  Class* X = class_linker_->FindClass(soa.Self(), "LX;", class_loader);
  Class* Y = class_linker_->FindClass(soa.Self(), "LY;", class_loader);
  ASSERT_TRUE(X != NULL);
  ASSERT_TRUE(Y != NULL);

  Handle<Object> x(hs.NewHandle(X->AllocObject(soa.Self())));
  Handle<Object> y(hs.NewHandle(Y->AllocObject(soa.Self())));
  ASSERT_TRUE(x.Get() != NULL);
  ASSERT_TRUE(y.Get() != NULL);

  EXPECT_TRUE(x->InstanceOf(X));
  EXPECT_FALSE(x->InstanceOf(Y));
  EXPECT_TRUE(y->InstanceOf(X));
  EXPECT_TRUE(y->InstanceOf(Y));

  Class* java_lang_Class = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Class;");
  Class* Object_array_class = class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;");

  EXPECT_FALSE(java_lang_Class->InstanceOf(Object_array_class));
  EXPECT_TRUE(Object_array_class->InstanceOf(java_lang_Class));

  // All array classes implement Cloneable and Serializable.
  Object* array = ObjectArray<Object>::Alloc(soa.Self(), Object_array_class, 1);
  Class* java_lang_Cloneable = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Cloneable;");
  Class* java_io_Serializable = class_linker_->FindSystemClass(soa.Self(), "Ljava/io/Serializable;");
  EXPECT_TRUE(array->InstanceOf(java_lang_Cloneable));
  EXPECT_TRUE(array->InstanceOf(java_io_Serializable));
}

TEST_F(ObjectTest, IsAssignableFrom) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("XandY");
  StackHandleScope<1> hs(soa.Self());
  Handle<ClassLoader> class_loader(hs.NewHandle(soa.Decode<ClassLoader*>(jclass_loader)));
  Class* X = class_linker_->FindClass(soa.Self(), "LX;", class_loader);
  Class* Y = class_linker_->FindClass(soa.Self(), "LY;", class_loader);

  EXPECT_TRUE(X->IsAssignableFrom(X));
  EXPECT_TRUE(X->IsAssignableFrom(Y));
  EXPECT_FALSE(Y->IsAssignableFrom(X));
  EXPECT_TRUE(Y->IsAssignableFrom(Y));

  // class final String implements CharSequence, ..
  Class* string = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/String;");
  Class* charseq = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/CharSequence;");
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
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("XandY");
  StackHandleScope<1> hs(soa.Self());
  Handle<ClassLoader> class_loader(hs.NewHandle(soa.Decode<ClassLoader*>(jclass_loader)));
  Class* X = class_linker_->FindClass(soa.Self(), "LX;", class_loader);
  Class* Y = class_linker_->FindClass(soa.Self(), "LY;", class_loader);
  ASSERT_TRUE(X != NULL);
  ASSERT_TRUE(Y != NULL);

  Class* YA = class_linker_->FindClass(soa.Self(), "[LY;", class_loader);
  Class* YAA = class_linker_->FindClass(soa.Self(), "[[LY;", class_loader);
  ASSERT_TRUE(YA != NULL);
  ASSERT_TRUE(YAA != NULL);

  Class* XAA = class_linker_->FindClass(soa.Self(), "[[LX;", class_loader);
  ASSERT_TRUE(XAA != NULL);

  Class* O = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;");
  Class* OA = class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;");
  Class* OAA = class_linker_->FindSystemClass(soa.Self(), "[[Ljava/lang/Object;");
  Class* OAAA = class_linker_->FindSystemClass(soa.Self(), "[[[Ljava/lang/Object;");
  ASSERT_TRUE(O != NULL);
  ASSERT_TRUE(OA != NULL);
  ASSERT_TRUE(OAA != NULL);
  ASSERT_TRUE(OAAA != NULL);

  Class* S = class_linker_->FindSystemClass(soa.Self(), "Ljava/io/Serializable;");
  Class* SA = class_linker_->FindSystemClass(soa.Self(), "[Ljava/io/Serializable;");
  Class* SAA = class_linker_->FindSystemClass(soa.Self(), "[[Ljava/io/Serializable;");
  ASSERT_TRUE(S != NULL);
  ASSERT_TRUE(SA != NULL);
  ASSERT_TRUE(SAA != NULL);

  Class* IA = class_linker_->FindSystemClass(soa.Self(), "[I");
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
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<String> s(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "ABC")));
  ASSERT_TRUE(s.Get() != NULL);
  Class* c = s->GetClass();
  ASSERT_TRUE(c != NULL);

  // Wrong type.
  EXPECT_TRUE(c->FindDeclaredInstanceField("count", "J") == NULL);
  EXPECT_TRUE(c->FindInstanceField("count", "J") == NULL);

  // Wrong name.
  EXPECT_TRUE(c->FindDeclaredInstanceField("Count", "I") == NULL);
  EXPECT_TRUE(c->FindInstanceField("Count", "I") == NULL);

  // Right name and type.
  ArtField* f1 = c->FindDeclaredInstanceField("count", "I");
  ArtField* f2 = c->FindInstanceField("count", "I");
  EXPECT_TRUE(f1 != NULL);
  EXPECT_TRUE(f2 != NULL);
  EXPECT_EQ(f1, f2);

  // TODO: check that s.count == 3.

  // Ensure that we handle superclass fields correctly...
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/StringBuilder;");
  ASSERT_TRUE(c != NULL);
  // No StringBuilder.count...
  EXPECT_TRUE(c->FindDeclaredInstanceField("count", "I") == NULL);
  // ...but there is an AbstractStringBuilder.count.
  EXPECT_TRUE(c->FindInstanceField("count", "I") != NULL);
}

TEST_F(ObjectTest, FindStaticField) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<String> s(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "ABC")));
  ASSERT_TRUE(s.Get() != NULL);
  Class* c = s->GetClass();
  ASSERT_TRUE(c != NULL);

  // Wrong type.
  EXPECT_TRUE(c->FindDeclaredStaticField("CASE_INSENSITIVE_ORDER", "I") == NULL);
  EXPECT_TRUE(c->FindStaticField("CASE_INSENSITIVE_ORDER", "I") == NULL);

  // Wrong name.
  EXPECT_TRUE(c->FindDeclaredStaticField("cASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;") == NULL);
  EXPECT_TRUE(c->FindStaticField("cASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;") == NULL);

  // Right name and type.
  ArtField* f1 = c->FindDeclaredStaticField("CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  ArtField* f2 = c->FindStaticField("CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_TRUE(f1 != NULL);
  EXPECT_TRUE(f2 != NULL);
  EXPECT_EQ(f1, f2);

  // TODO: test static fields via superclasses.
  // TODO: test static fields via interfaces.
  // TODO: test that interfaces trump superclasses.
}

}  // namespace mirror
}  // namespace art
