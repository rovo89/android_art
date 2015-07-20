/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not read this file except in compliance with the License.
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

#include "gtest/gtest.h"
#include "nodes.h"
#include "primitive.h"

namespace art {

/**
 * Tests for the SideEffects class.
 */

//
// Helper methods.
//

void testWriteAndReadSanity(SideEffects write, SideEffects read) {
  EXPECT_FALSE(write.DoesNothing());
  EXPECT_FALSE(read.DoesNothing());

  EXPECT_TRUE(write.DoesAnyWrite());
  EXPECT_FALSE(write.DoesAnyRead());
  EXPECT_FALSE(read.DoesAnyWrite());
  EXPECT_TRUE(read.DoesAnyRead());

  // All-dependences.
  SideEffects all = SideEffects::All();
  EXPECT_TRUE(all.MayDependOn(write));
  EXPECT_FALSE(write.MayDependOn(all));
  EXPECT_FALSE(all.MayDependOn(read));
  EXPECT_TRUE(read.MayDependOn(all));

  // None-dependences.
  SideEffects none = SideEffects::None();
  EXPECT_FALSE(none.MayDependOn(write));
  EXPECT_FALSE(write.MayDependOn(none));
  EXPECT_FALSE(none.MayDependOn(read));
  EXPECT_FALSE(read.MayDependOn(none));
}

void testWriteAndReadDependence(SideEffects write, SideEffects read) {
  testWriteAndReadSanity(write, read);

  // Dependence only in one direction.
  EXPECT_FALSE(write.MayDependOn(read));
  EXPECT_TRUE(read.MayDependOn(write));
}

void testNoWriteAndReadDependence(SideEffects write, SideEffects read) {
  testWriteAndReadSanity(write, read);

  // No dependence in any direction.
  EXPECT_FALSE(write.MayDependOn(read));
  EXPECT_FALSE(read.MayDependOn(write));
}

//
// Actual tests.
//

TEST(SideEffectsTest, All) {
  SideEffects all = SideEffects::All();
  EXPECT_TRUE(all.DoesAnyWrite());
  EXPECT_TRUE(all.DoesAnyRead());
  EXPECT_FALSE(all.DoesNothing());
  EXPECT_TRUE(all.DoesAll());
}

TEST(SideEffectsTest, None) {
  SideEffects none = SideEffects::None();
  EXPECT_FALSE(none.DoesAnyWrite());
  EXPECT_FALSE(none.DoesAnyRead());
  EXPECT_TRUE(none.DoesNothing());
  EXPECT_FALSE(none.DoesAll());
}

TEST(SideEffectsTest, DependencesAndNoDependences) {
  // Apply test to each individual primitive type.
  for (Primitive::Type type = Primitive::kPrimNot;
      type < Primitive::kPrimVoid;
      type = Primitive::Type(type + 1)) {
    // Same primitive type and access type: proper write/read dep.
    testWriteAndReadDependence(
        SideEffects::FieldWriteOfType(type),
        SideEffects::FieldReadOfType(type));
    testWriteAndReadDependence(
        SideEffects::ArrayWriteOfType(type),
        SideEffects::ArrayReadOfType(type));
    // Same primitive type but different access type: no write/read dep.
    testNoWriteAndReadDependence(
        SideEffects::FieldWriteOfType(type),
        SideEffects::ArrayReadOfType(type));
    testNoWriteAndReadDependence(
        SideEffects::ArrayWriteOfType(type),
        SideEffects::FieldReadOfType(type));
  }
}

TEST(SideEffectsTest, NoDependences) {
  // Different primitive type, same access type: no write/read dep.
  testNoWriteAndReadDependence(
      SideEffects::FieldWriteOfType(Primitive::kPrimInt),
      SideEffects::FieldReadOfType(Primitive::kPrimDouble));
  testNoWriteAndReadDependence(
      SideEffects::ArrayWriteOfType(Primitive::kPrimInt),
      SideEffects::ArrayReadOfType(Primitive::kPrimDouble));
  // Everything different: no write/read dep.
  testNoWriteAndReadDependence(
      SideEffects::FieldWriteOfType(Primitive::kPrimInt),
      SideEffects::ArrayReadOfType(Primitive::kPrimDouble));
  testNoWriteAndReadDependence(
      SideEffects::ArrayWriteOfType(Primitive::kPrimInt),
      SideEffects::FieldReadOfType(Primitive::kPrimDouble));
}

TEST(SideEffectsTest, SameWidthTypes) {
  // Type I/F.
  testWriteAndReadDependence(
      SideEffects::FieldWriteOfType(Primitive::kPrimInt),
      SideEffects::FieldReadOfType(Primitive::kPrimFloat));
  testWriteAndReadDependence(
      SideEffects::ArrayWriteOfType(Primitive::kPrimInt),
      SideEffects::ArrayReadOfType(Primitive::kPrimFloat));
  // Type L/D.
  testWriteAndReadDependence(
      SideEffects::FieldWriteOfType(Primitive::kPrimLong),
      SideEffects::FieldReadOfType(Primitive::kPrimDouble));
  testWriteAndReadDependence(
      SideEffects::ArrayWriteOfType(Primitive::kPrimLong),
      SideEffects::ArrayReadOfType(Primitive::kPrimDouble));
}

TEST(SideEffectsTest, AllWritesAndReads) {
  SideEffects s = SideEffects::None();
  // Keep taking the union of different writes and reads.
  for (Primitive::Type type = Primitive::kPrimNot;
        type < Primitive::kPrimVoid;
        type = Primitive::Type(type + 1)) {
    s = s.Union(SideEffects::FieldWriteOfType(type));
    s = s.Union(SideEffects::ArrayWriteOfType(type));
    s = s.Union(SideEffects::FieldReadOfType(type));
    s = s.Union(SideEffects::ArrayReadOfType(type));
  }
  EXPECT_TRUE(s.DoesAll());
}

TEST(SideEffectsTest, BitStrings) {
  EXPECT_STREQ(
      "|||||",
      SideEffects::None().ToString().c_str());
  EXPECT_STREQ(
      "|DFJISCBZL|DFJISCBZL|DFJISCBZL|DFJISCBZL|",
      SideEffects::All().ToString().c_str());
  EXPECT_STREQ(
      "||||L|",
      SideEffects::FieldWriteOfType(Primitive::kPrimNot).ToString().c_str());
  EXPECT_STREQ(
      "|||Z||",
      SideEffects::ArrayWriteOfType(Primitive::kPrimBoolean).ToString().c_str());
  EXPECT_STREQ(
      "||B|||",
      SideEffects::FieldReadOfType(Primitive::kPrimByte).ToString().c_str());
  EXPECT_STREQ(
      "|DJ||||",  // note: DJ alias
      SideEffects::ArrayReadOfType(Primitive::kPrimDouble).ToString().c_str());
  SideEffects s = SideEffects::None();
  s = s.Union(SideEffects::FieldWriteOfType(Primitive::kPrimChar));
  s = s.Union(SideEffects::FieldWriteOfType(Primitive::kPrimLong));
  s = s.Union(SideEffects::ArrayWriteOfType(Primitive::kPrimShort));
  s = s.Union(SideEffects::FieldReadOfType(Primitive::kPrimInt));
  s = s.Union(SideEffects::ArrayReadOfType(Primitive::kPrimFloat));
  s = s.Union(SideEffects::ArrayReadOfType(Primitive::kPrimDouble));
  EXPECT_STREQ(
      "|DFJI|FI|S|DJC|",   // note: DJ/FI alias.
      s.ToString().c_str());
}

}  // namespace art
