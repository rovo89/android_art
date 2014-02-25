/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "common_test.h"
#include "invoke_arg_array_builder.h"
#include "mirror/array-inl.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "transaction.h"

namespace art {

class TransactionTest : public CommonTest {
};

TEST_F(TransactionTest, Object_class) {
  ScopedObjectAccess soa(Thread::Current());
  SirtRef<mirror::Class> sirt_klass(soa.Self(),
                                    class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;"));
  ASSERT_TRUE(sirt_klass.get() != nullptr);

  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  SirtRef<mirror::Object> sirt_obj(soa.Self(), sirt_klass->AllocObject(soa.Self()));
  ASSERT_TRUE(sirt_obj.get() != nullptr);
  ASSERT_EQ(sirt_obj->GetClass(), sirt_klass.get());
  Runtime::Current()->ExitTransactionMode();

  // Aborting transaction must not clear the Object::class field.
  transaction.Abort();
  EXPECT_EQ(sirt_obj->GetClass(), sirt_klass.get());
}

TEST_F(TransactionTest, Object_monitor) {
  ScopedObjectAccess soa(Thread::Current());
  SirtRef<mirror::Class> sirt_klass(soa.Self(),
                                    class_linker_->FindSystemClass(soa.Self(),
                                                                   "Ljava/lang/Object;"));
  ASSERT_TRUE(sirt_klass.get() != nullptr);
  SirtRef<mirror::Object> sirt_obj(soa.Self(), sirt_klass->AllocObject(soa.Self()));
  ASSERT_TRUE(sirt_obj.get() != nullptr);
  ASSERT_EQ(sirt_obj->GetClass(), sirt_klass.get());

  // Lock object's monitor outside the transaction.
  sirt_obj->MonitorEnter(soa.Self());
  uint32_t old_lock_word = sirt_obj->GetLockWord().GetValue();

  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  // Unlock object's monitor inside the transaction.
  sirt_obj->MonitorExit(soa.Self());
  uint32_t new_lock_word = sirt_obj->GetLockWord().GetValue();
  Runtime::Current()->ExitTransactionMode();

  // Aborting transaction must not clear the Object::class field.
  transaction.Abort();
  uint32_t aborted_lock_word = sirt_obj->GetLockWord().GetValue();
  EXPECT_NE(old_lock_word, new_lock_word);
  EXPECT_EQ(aborted_lock_word, new_lock_word);
}

TEST_F(TransactionTest, Array_length) {
  ScopedObjectAccess soa(Thread::Current());
  SirtRef<mirror::Class> sirt_klass(soa.Self(),
                                    class_linker_->FindSystemClass(soa.Self(),
                                                                   "[Ljava/lang/Object;"));
  ASSERT_TRUE(sirt_klass.get() != nullptr);

  constexpr int32_t kArraySize = 2;

  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);

  // Allocate an array during transaction.
  SirtRef<mirror::Array> sirt_obj(soa.Self(),
                                  mirror::Array::Alloc<false>(soa.Self(), sirt_klass.get(), kArraySize));
  ASSERT_TRUE(sirt_obj.get() != nullptr);
  ASSERT_EQ(sirt_obj->GetClass(), sirt_klass.get());
  Runtime::Current()->ExitTransactionMode();

  // Aborting transaction must not clear the Object::class field.
  transaction.Abort();
  EXPECT_EQ(sirt_obj->GetLength(), kArraySize);
}

TEST_F(TransactionTest, StaticFieldsTest) {
  ScopedObjectAccess soa(Thread::Current());
  SirtRef<mirror::ClassLoader> class_loader(
      soa.Self(), soa.Decode<mirror::ClassLoader*>(LoadDex("Transaction")));
  ASSERT_TRUE(class_loader.get() != nullptr);

  SirtRef<mirror::Class> sirt_klass(soa.Self(),
                                    class_linker_->FindClass(soa.Self(), "LStaticFieldsTest;",
                                                             class_loader));
  ASSERT_TRUE(sirt_klass.get() != nullptr);
  class_linker_->EnsureInitialized(sirt_klass, true, true);
  ASSERT_TRUE(sirt_klass->IsInitialized());

  // Lookup fields.
  mirror::ArtField* booleanField = sirt_klass->FindDeclaredStaticField("booleanField", "Z");
  ASSERT_TRUE(booleanField != nullptr);
  ASSERT_EQ(FieldHelper(booleanField).GetTypeAsPrimitiveType(), Primitive::kPrimBoolean);
  ASSERT_EQ(booleanField->GetBoolean(sirt_klass.get()), false);

  mirror::ArtField* byteField = sirt_klass->FindDeclaredStaticField("byteField", "B");
  ASSERT_TRUE(byteField != nullptr);
  ASSERT_EQ(FieldHelper(byteField).GetTypeAsPrimitiveType(), Primitive::kPrimByte);
  ASSERT_EQ(byteField->GetByte(sirt_klass.get()), 0);

  mirror::ArtField* charField = sirt_klass->FindDeclaredStaticField("charField", "C");
  ASSERT_TRUE(charField != nullptr);
  ASSERT_EQ(FieldHelper(charField).GetTypeAsPrimitiveType(), Primitive::kPrimChar);
  ASSERT_EQ(charField->GetChar(sirt_klass.get()), 0u);

  mirror::ArtField* shortField = sirt_klass->FindDeclaredStaticField("shortField", "S");
  ASSERT_TRUE(shortField != nullptr);
  ASSERT_EQ(FieldHelper(shortField).GetTypeAsPrimitiveType(), Primitive::kPrimShort);
  ASSERT_EQ(shortField->GetShort(sirt_klass.get()), 0);

  mirror::ArtField* intField = sirt_klass->FindDeclaredStaticField("intField", "I");
  ASSERT_TRUE(intField != nullptr);
  ASSERT_EQ(FieldHelper(intField).GetTypeAsPrimitiveType(), Primitive::kPrimInt);
  ASSERT_EQ(intField->GetInt(sirt_klass.get()), 0);

  mirror::ArtField* longField = sirt_klass->FindDeclaredStaticField("longField", "J");
  ASSERT_TRUE(longField != nullptr);
  ASSERT_EQ(FieldHelper(longField).GetTypeAsPrimitiveType(), Primitive::kPrimLong);
  ASSERT_EQ(longField->GetLong(sirt_klass.get()), static_cast<int64_t>(0));

  mirror::ArtField* floatField = sirt_klass->FindDeclaredStaticField("floatField", "F");
  ASSERT_TRUE(floatField != nullptr);
  ASSERT_EQ(FieldHelper(floatField).GetTypeAsPrimitiveType(), Primitive::kPrimFloat);
  ASSERT_EQ(floatField->GetFloat(sirt_klass.get()), static_cast<float>(0.0f));

  mirror::ArtField* doubleField = sirt_klass->FindDeclaredStaticField("doubleField", "D");
  ASSERT_TRUE(doubleField != nullptr);
  ASSERT_EQ(FieldHelper(doubleField).GetTypeAsPrimitiveType(), Primitive::kPrimDouble);
  ASSERT_EQ(doubleField->GetDouble(sirt_klass.get()), static_cast<double>(0.0));

  mirror::ArtField* objectField = sirt_klass->FindDeclaredStaticField("objectField",
                                                                      "Ljava/lang/Object;");
  ASSERT_TRUE(objectField != nullptr);
  ASSERT_EQ(FieldHelper(objectField).GetTypeAsPrimitiveType(), Primitive::kPrimNot);
  ASSERT_EQ(objectField->GetObject(sirt_klass.get()), nullptr);

  // Create a java.lang.Object instance to set objectField.
  SirtRef<mirror::Class> object_klass(soa.Self(),
                                      class_linker_->FindSystemClass(soa.Self(),
                                                                     "Ljava/lang/Object;"));
  ASSERT_TRUE(object_klass.get() != nullptr);
  SirtRef<mirror::Object> sirt_obj(soa.Self(), sirt_klass->AllocObject(soa.Self()));
  ASSERT_TRUE(sirt_obj.get() != nullptr);
  ASSERT_EQ(sirt_obj->GetClass(), sirt_klass.get());

  // Modify fields inside transaction and abort it.
  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  booleanField->SetBoolean<true>(sirt_klass.get(), true);
  byteField->SetByte<true>(sirt_klass.get(), 1);
  charField->SetChar<true>(sirt_klass.get(), 1u);
  shortField->SetShort<true>(sirt_klass.get(), 1);
  intField->SetInt<true>(sirt_klass.get(), 1);
  longField->SetLong<true>(sirt_klass.get(), 1);
  floatField->SetFloat<true>(sirt_klass.get(), 1.0);
  doubleField->SetDouble<true>(sirt_klass.get(), 1.0);
  objectField->SetObject<true>(sirt_klass.get(), sirt_obj.get());
  Runtime::Current()->ExitTransactionMode();
  transaction.Abort();

  // Check values have properly been restored to their original (default) value.
  EXPECT_EQ(booleanField->GetBoolean(sirt_klass.get()), false);
  EXPECT_EQ(byteField->GetByte(sirt_klass.get()), 0);
  EXPECT_EQ(charField->GetChar(sirt_klass.get()), 0u);
  EXPECT_EQ(shortField->GetShort(sirt_klass.get()), 0);
  EXPECT_EQ(intField->GetInt(sirt_klass.get()), 0);
  EXPECT_EQ(longField->GetLong(sirt_klass.get()), static_cast<int64_t>(0));
  EXPECT_EQ(floatField->GetFloat(sirt_klass.get()), static_cast<float>(0.0f));
  EXPECT_EQ(doubleField->GetDouble(sirt_klass.get()), static_cast<double>(0.0));
  EXPECT_EQ(objectField->GetObject(sirt_klass.get()), nullptr);
}

TEST_F(TransactionTest, InstanceFieldsTest) {
  ScopedObjectAccess soa(Thread::Current());
  SirtRef<mirror::ClassLoader> class_loader(
      soa.Self(), soa.Decode<mirror::ClassLoader*>(LoadDex("Transaction")));
  ASSERT_TRUE(class_loader.get() != nullptr);

  SirtRef<mirror::Class> sirt_klass(soa.Self(),
                                    class_linker_->FindClass(soa.Self(), "LInstanceFieldsTest;",
                                                             class_loader));
  ASSERT_TRUE(sirt_klass.get() != nullptr);
  class_linker_->EnsureInitialized(sirt_klass, true, true);
  ASSERT_TRUE(sirt_klass->IsInitialized());

  // Allocate an InstanceFieldTest object.
  SirtRef<mirror::Object> sirt_instance(soa.Self(), sirt_klass->AllocObject(soa.Self()));
  ASSERT_TRUE(sirt_instance.get() != nullptr);

  // Lookup fields.
  mirror::ArtField* booleanField = sirt_klass->FindDeclaredInstanceField("booleanField", "Z");
  ASSERT_TRUE(booleanField != nullptr);
  ASSERT_EQ(FieldHelper(booleanField).GetTypeAsPrimitiveType(), Primitive::kPrimBoolean);
  ASSERT_EQ(booleanField->GetBoolean(sirt_instance.get()), false);

  mirror::ArtField* byteField = sirt_klass->FindDeclaredInstanceField("byteField", "B");
  ASSERT_TRUE(byteField != nullptr);
  ASSERT_EQ(FieldHelper(byteField).GetTypeAsPrimitiveType(), Primitive::kPrimByte);
  ASSERT_EQ(byteField->GetByte(sirt_instance.get()), 0);

  mirror::ArtField* charField = sirt_klass->FindDeclaredInstanceField("charField", "C");
  ASSERT_TRUE(charField != nullptr);
  ASSERT_EQ(FieldHelper(charField).GetTypeAsPrimitiveType(), Primitive::kPrimChar);
  ASSERT_EQ(charField->GetChar(sirt_instance.get()), 0u);

  mirror::ArtField* shortField = sirt_klass->FindDeclaredInstanceField("shortField", "S");
  ASSERT_TRUE(shortField != nullptr);
  ASSERT_EQ(FieldHelper(shortField).GetTypeAsPrimitiveType(), Primitive::kPrimShort);
  ASSERT_EQ(shortField->GetShort(sirt_instance.get()), 0);

  mirror::ArtField* intField = sirt_klass->FindDeclaredInstanceField("intField", "I");
  ASSERT_TRUE(intField != nullptr);
  ASSERT_EQ(FieldHelper(intField).GetTypeAsPrimitiveType(), Primitive::kPrimInt);
  ASSERT_EQ(intField->GetInt(sirt_instance.get()), 0);

  mirror::ArtField* longField = sirt_klass->FindDeclaredInstanceField("longField", "J");
  ASSERT_TRUE(longField != nullptr);
  ASSERT_EQ(FieldHelper(longField).GetTypeAsPrimitiveType(), Primitive::kPrimLong);
  ASSERT_EQ(longField->GetLong(sirt_instance.get()), static_cast<int64_t>(0));

  mirror::ArtField* floatField = sirt_klass->FindDeclaredInstanceField("floatField", "F");
  ASSERT_TRUE(floatField != nullptr);
  ASSERT_EQ(FieldHelper(floatField).GetTypeAsPrimitiveType(), Primitive::kPrimFloat);
  ASSERT_EQ(floatField->GetFloat(sirt_instance.get()), static_cast<float>(0.0f));

  mirror::ArtField* doubleField = sirt_klass->FindDeclaredInstanceField("doubleField", "D");
  ASSERT_TRUE(doubleField != nullptr);
  ASSERT_EQ(FieldHelper(doubleField).GetTypeAsPrimitiveType(), Primitive::kPrimDouble);
  ASSERT_EQ(doubleField->GetDouble(sirt_instance.get()), static_cast<double>(0.0));

  mirror::ArtField* objectField = sirt_klass->FindDeclaredInstanceField("objectField",
                                                                        "Ljava/lang/Object;");
  ASSERT_TRUE(objectField != nullptr);
  ASSERT_EQ(FieldHelper(objectField).GetTypeAsPrimitiveType(), Primitive::kPrimNot);
  ASSERT_EQ(objectField->GetObject(sirt_instance.get()), nullptr);

  // Create a java.lang.Object instance to set objectField.
  SirtRef<mirror::Class> object_klass(soa.Self(),
                                      class_linker_->FindSystemClass(soa.Self(),
                                                                     "Ljava/lang/Object;"));
  ASSERT_TRUE(object_klass.get() != nullptr);
  SirtRef<mirror::Object> sirt_obj(soa.Self(), sirt_klass->AllocObject(soa.Self()));
  ASSERT_TRUE(sirt_obj.get() != nullptr);
  ASSERT_EQ(sirt_obj->GetClass(), sirt_klass.get());

  // Modify fields inside transaction and abort it.
  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  booleanField->SetBoolean<true>(sirt_instance.get(), true);
  byteField->SetByte<true>(sirt_instance.get(), 1);
  charField->SetChar<true>(sirt_instance.get(), 1u);
  shortField->SetShort<true>(sirt_instance.get(), 1);
  intField->SetInt<true>(sirt_instance.get(), 1);
  longField->SetLong<true>(sirt_instance.get(), 1);
  floatField->SetFloat<true>(sirt_instance.get(), 1.0);
  doubleField->SetDouble<true>(sirt_instance.get(), 1.0);
  objectField->SetObject<true>(sirt_instance.get(), sirt_obj.get());
  Runtime::Current()->ExitTransactionMode();
  transaction.Abort();

  // Check values have properly been restored to their original (default) value.
  EXPECT_EQ(booleanField->GetBoolean(sirt_instance.get()), false);
  EXPECT_EQ(byteField->GetByte(sirt_instance.get()), 0);
  EXPECT_EQ(charField->GetChar(sirt_instance.get()), 0u);
  EXPECT_EQ(shortField->GetShort(sirt_instance.get()), 0);
  EXPECT_EQ(intField->GetInt(sirt_instance.get()), 0);
  EXPECT_EQ(longField->GetLong(sirt_instance.get()), static_cast<int64_t>(0));
  EXPECT_EQ(floatField->GetFloat(sirt_instance.get()), static_cast<float>(0.0f));
  EXPECT_EQ(doubleField->GetDouble(sirt_instance.get()), static_cast<double>(0.0));
  EXPECT_EQ(objectField->GetObject(sirt_instance.get()), nullptr);
}


TEST_F(TransactionTest, StaticArrayFieldsTest) {
  ScopedObjectAccess soa(Thread::Current());
  SirtRef<mirror::ClassLoader> class_loader(
      soa.Self(), soa.Decode<mirror::ClassLoader*>(LoadDex("Transaction")));
  ASSERT_TRUE(class_loader.get() != nullptr);

  SirtRef<mirror::Class> sirt_klass(soa.Self(),
                                    class_linker_->FindClass(soa.Self(), "LStaticArrayFieldsTest;",
                                                             class_loader));
  ASSERT_TRUE(sirt_klass.get() != nullptr);
  class_linker_->EnsureInitialized(sirt_klass, true, true);
  ASSERT_TRUE(sirt_klass->IsInitialized());

  // Lookup fields.
  mirror::ArtField* booleanArrayField = sirt_klass->FindDeclaredStaticField("booleanArrayField", "[Z");
  ASSERT_TRUE(booleanArrayField != nullptr);
  mirror::BooleanArray* booleanArray = booleanArrayField->GetObject(sirt_klass.get())->AsBooleanArray();
  ASSERT_TRUE(booleanArray != nullptr);
  ASSERT_EQ(booleanArray->GetLength(), 1);
  ASSERT_EQ(booleanArray->GetWithoutChecks(0), false);

  mirror::ArtField* byteArrayField = sirt_klass->FindDeclaredStaticField("byteArrayField", "[B");
  ASSERT_TRUE(byteArrayField != nullptr);
  mirror::ByteArray* byteArray = byteArrayField->GetObject(sirt_klass.get())->AsByteArray();
  ASSERT_TRUE(byteArray != nullptr);
  ASSERT_EQ(byteArray->GetLength(), 1);
  ASSERT_EQ(byteArray->GetWithoutChecks(0), 0);

  mirror::ArtField* charArrayField = sirt_klass->FindDeclaredStaticField("charArrayField", "[C");
  ASSERT_TRUE(charArrayField != nullptr);
  mirror::CharArray* charArray = charArrayField->GetObject(sirt_klass.get())->AsCharArray();
  ASSERT_TRUE(charArray != nullptr);
  ASSERT_EQ(charArray->GetLength(), 1);
  ASSERT_EQ(charArray->GetWithoutChecks(0), 0u);

  mirror::ArtField* shortArrayField = sirt_klass->FindDeclaredStaticField("shortArrayField", "[S");
  ASSERT_TRUE(shortArrayField != nullptr);
  mirror::ShortArray* shortArray = shortArrayField->GetObject(sirt_klass.get())->AsShortArray();
  ASSERT_TRUE(shortArray != nullptr);
  ASSERT_EQ(shortArray->GetLength(), 1);
  ASSERT_EQ(shortArray->GetWithoutChecks(0), 0);

  mirror::ArtField* intArrayField = sirt_klass->FindDeclaredStaticField("intArrayField", "[I");
  ASSERT_TRUE(intArrayField != nullptr);
  mirror::IntArray* intArray = intArrayField->GetObject(sirt_klass.get())->AsIntArray();
  ASSERT_TRUE(intArray != nullptr);
  ASSERT_EQ(intArray->GetLength(), 1);
  ASSERT_EQ(intArray->GetWithoutChecks(0), 0);

  mirror::ArtField* longArrayField = sirt_klass->FindDeclaredStaticField("longArrayField", "[J");
  ASSERT_TRUE(longArrayField != nullptr);
  mirror::LongArray* longArray = longArrayField->GetObject(sirt_klass.get())->AsLongArray();
  ASSERT_TRUE(longArray != nullptr);
  ASSERT_EQ(longArray->GetLength(), 1);
  ASSERT_EQ(longArray->GetWithoutChecks(0), static_cast<int64_t>(0));

  mirror::ArtField* floatArrayField = sirt_klass->FindDeclaredStaticField("floatArrayField", "[F");
  ASSERT_TRUE(floatArrayField != nullptr);
  mirror::FloatArray* floatArray = floatArrayField->GetObject(sirt_klass.get())->AsFloatArray();
  ASSERT_TRUE(floatArray != nullptr);
  ASSERT_EQ(floatArray->GetLength(), 1);
  ASSERT_EQ(floatArray->GetWithoutChecks(0), static_cast<float>(0.0f));

  mirror::ArtField* doubleArrayField = sirt_klass->FindDeclaredStaticField("doubleArrayField", "[D");
  ASSERT_TRUE(doubleArrayField != nullptr);
  mirror::DoubleArray* doubleArray = doubleArrayField->GetObject(sirt_klass.get())->AsDoubleArray();
  ASSERT_TRUE(doubleArray != nullptr);
  ASSERT_EQ(doubleArray->GetLength(), 1);
  ASSERT_EQ(doubleArray->GetWithoutChecks(0), static_cast<double>(0.0f));

  mirror::ArtField* objectArrayField = sirt_klass->FindDeclaredStaticField("objectArrayField",
                                                                           "[Ljava/lang/Object;");
  ASSERT_TRUE(objectArrayField != nullptr);
  mirror::ObjectArray<mirror::Object>* objectArray =
      objectArrayField->GetObject(sirt_klass.get())->AsObjectArray<mirror::Object>();
  ASSERT_TRUE(objectArray != nullptr);
  ASSERT_EQ(objectArray->GetLength(), 1);
  ASSERT_EQ(objectArray->GetWithoutChecks(0), nullptr);

  // Create a java.lang.Object instance to set objectField.
  SirtRef<mirror::Class> object_klass(soa.Self(),
                                      class_linker_->FindSystemClass(soa.Self(),
                                                                     "Ljava/lang/Object;"));
  ASSERT_TRUE(object_klass.get() != nullptr);
  SirtRef<mirror::Object> sirt_obj(soa.Self(), sirt_klass->AllocObject(soa.Self()));
  ASSERT_TRUE(sirt_obj.get() != nullptr);
  ASSERT_EQ(sirt_obj->GetClass(), sirt_klass.get());

  // Modify fields inside transaction and abort it.
  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  booleanArray->SetWithoutChecks<true>(0, true);
  byteArray->SetWithoutChecks<true>(0, 1);
  charArray->SetWithoutChecks<true>(0, 1u);
  shortArray->SetWithoutChecks<true>(0, 1);
  intArray->SetWithoutChecks<true>(0, 1);
  longArray->SetWithoutChecks<true>(0, 1);
  floatArray->SetWithoutChecks<true>(0, 1.0);
  doubleArray->SetWithoutChecks<true>(0, 1.0);
  objectArray->SetWithoutChecks<true>(0, sirt_obj.get());
  Runtime::Current()->ExitTransactionMode();
  transaction.Abort();

  // Check values have properly been restored to their original (default) value.
  EXPECT_EQ(booleanArray->GetWithoutChecks(0), false);
  EXPECT_EQ(byteArray->GetWithoutChecks(0), 0);
  EXPECT_EQ(charArray->GetWithoutChecks(0), 0u);
  EXPECT_EQ(shortArray->GetWithoutChecks(0), 0);
  EXPECT_EQ(intArray->GetWithoutChecks(0), 0);
  EXPECT_EQ(longArray->GetWithoutChecks(0), static_cast<int64_t>(0));
  EXPECT_EQ(floatArray->GetWithoutChecks(0), static_cast<float>(0.0f));
  EXPECT_EQ(doubleArray->GetWithoutChecks(0), static_cast<double>(0.0f));
  EXPECT_EQ(objectArray->GetWithoutChecks(0), nullptr);
}

TEST_F(TransactionTest, EmptyClass) {
  ScopedObjectAccess soa(Thread::Current());
  SirtRef<mirror::ClassLoader> class_loader(
      soa.Self(), soa.Decode<mirror::ClassLoader*>(LoadDex("Transaction")));
  ASSERT_TRUE(class_loader.get() != nullptr);

  SirtRef<mirror::Class> sirt_klass(soa.Self(),
                                    class_linker_->FindClass(soa.Self(),
                                                             "LTransaction$EmptyStatic;",
                                                             class_loader));
  ASSERT_TRUE(sirt_klass.get() != nullptr);
  class_linker_->VerifyClass(sirt_klass);
  ASSERT_TRUE(sirt_klass->IsVerified());

  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  class_linker_->EnsureInitialized(sirt_klass, true, true);
  Runtime::Current()->ExitTransactionMode();
  ASSERT_FALSE(soa.Self()->IsExceptionPending());
}

TEST_F(TransactionTest, StaticFieldClass) {
  ScopedObjectAccess soa(Thread::Current());
  SirtRef<mirror::ClassLoader> class_loader(
      soa.Self(), soa.Decode<mirror::ClassLoader*>(LoadDex("Transaction")));
  ASSERT_TRUE(class_loader.get() != nullptr);

  SirtRef<mirror::Class> sirt_klass(soa.Self(),
                                    class_linker_->FindClass(soa.Self(),
                                                             "LTransaction$StaticFieldClass;",
                                                             class_loader));
  ASSERT_TRUE(sirt_klass.get() != nullptr);
  class_linker_->VerifyClass(sirt_klass);
  ASSERT_TRUE(sirt_klass->IsVerified());

  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  class_linker_->EnsureInitialized(sirt_klass, true, true);
  Runtime::Current()->ExitTransactionMode();
  ASSERT_FALSE(soa.Self()->IsExceptionPending());
}

TEST_F(TransactionTest, BlacklistedClass) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("Transaction");
  SirtRef<mirror::ClassLoader> class_loader(soa.Self(),
                                            soa.Decode<mirror::ClassLoader*>(jclass_loader));
  ASSERT_TRUE(class_loader.get() != nullptr);

  // Load and verify java.lang.ExceptionInInitializerError and java.lang.InternalError which will
  // be thrown by class initialization due to native call.
  SirtRef<mirror::Class> sirt_klass(soa.Self(),
                                    class_linker_->FindSystemClass(soa.Self(),
                                                                   "Ljava/lang/ExceptionInInitializerError;"));
  ASSERT_TRUE(sirt_klass.get() != nullptr);
  class_linker_->VerifyClass(sirt_klass);
  ASSERT_TRUE(sirt_klass->IsVerified());
  sirt_klass.reset(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/InternalError;"));
  ASSERT_TRUE(sirt_klass.get() != nullptr);
  class_linker_->VerifyClass(sirt_klass);
  ASSERT_TRUE(sirt_klass->IsVerified());

  // Load and verify Transaction$NativeSupport used in class initialization.
  sirt_klass.reset(class_linker_->FindClass(soa.Self(), "LTransaction$NativeSupport;",
                                            class_loader));
  ASSERT_TRUE(sirt_klass.get() != nullptr);
  class_linker_->VerifyClass(sirt_klass);
  ASSERT_TRUE(sirt_klass->IsVerified());

  sirt_klass.reset(class_linker_->FindClass(soa.Self(), "LTransaction$BlacklistedClass;",
                                            class_loader));
  ASSERT_TRUE(sirt_klass.get() != nullptr);
  class_linker_->VerifyClass(sirt_klass);
  ASSERT_TRUE(sirt_klass->IsVerified());

  Transaction transaction;
  Runtime::Current()->EnterTransactionMode(&transaction);
  class_linker_->EnsureInitialized(sirt_klass, true, true);
  Runtime::Current()->ExitTransactionMode();
  ASSERT_TRUE(soa.Self()->IsExceptionPending());
}


}  // namespace art
