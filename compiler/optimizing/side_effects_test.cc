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
  EXPECT_TRUE(all.DoesAllReadWrite());
}

TEST(SideEffectsTest, None) {
  SideEffects none = SideEffects::None();
  EXPECT_FALSE(none.DoesAnyWrite());
  EXPECT_FALSE(none.DoesAnyRead());
  EXPECT_TRUE(none.DoesNothing());
  EXPECT_FALSE(none.DoesAllReadWrite());
}

TEST(SideEffectsTest, DependencesAndNoDependences) {
  // Apply test to each individual primitive type.
  for (Primitive::Type type = Primitive::kPrimNot;
      type < Primitive::kPrimVoid;
      type = Primitive::Type(type + 1)) {
    // Same primitive type and access type: proper write/read dep.
    testWriteAndReadDependence(
        SideEffects::FieldWriteOfType(type, false),
        SideEffects::FieldReadOfType(type, false));
    testWriteAndReadDependence(
        SideEffects::ArrayWriteOfType(type),
        SideEffects::ArrayReadOfType(type));
    // Same primitive type but different access type: no write/read dep.
    testNoWriteAndReadDependence(
        SideEffects::FieldWriteOfType(type, false),
        SideEffects::ArrayReadOfType(type));
    testNoWriteAndReadDependence(
        SideEffects::ArrayWriteOfType(type),
        SideEffects::FieldReadOfType(type, false));
  }
}

TEST(SideEffectsTest, NoDependences) {
  // Different primitive type, same access type: no write/read dep.
  testNoWriteAndReadDependence(
      SideEffects::FieldWriteOfType(Primitive::kPrimInt, false),
      SideEffects::FieldReadOfType(Primitive::kPrimDouble, false));
  testNoWriteAndReadDependence(
      SideEffects::ArrayWriteOfType(Primitive::kPrimInt),
      SideEffects::ArrayReadOfType(Primitive::kPrimDouble));
  // Everything different: no write/read dep.
  testNoWriteAndReadDependence(
      SideEffects::FieldWriteOfType(Primitive::kPrimInt, false),
      SideEffects::ArrayReadOfType(Primitive::kPrimDouble));
  testNoWriteAndReadDependence(
      SideEffects::ArrayWriteOfType(Primitive::kPrimInt),
      SideEffects::FieldReadOfType(Primitive::kPrimDouble, false));
}

TEST(SideEffectsTest, VolatileDependences) {
  SideEffects volatile_write =
      SideEffects::FieldWriteOfType(Primitive::kPrimInt, /* is_volatile */ true);
  SideEffects any_write =
      SideEffects::FieldWriteOfType(Primitive::kPrimInt, /* is_volatile */ false);
  SideEffects volatile_read =
      SideEffects::FieldReadOfType(Primitive::kPrimByte, /* is_volatile */ true);
  SideEffects any_read =
      SideEffects::FieldReadOfType(Primitive::kPrimByte, /* is_volatile */ false);

  EXPECT_FALSE(volatile_write.MayDependOn(any_read));
  EXPECT_TRUE(any_read.MayDependOn(volatile_write));
  EXPECT_TRUE(volatile_write.MayDependOn(any_write));
  EXPECT_FALSE(any_write.MayDependOn(volatile_write));

  EXPECT_FALSE(volatile_read.MayDependOn(any_read));
  EXPECT_TRUE(any_read.MayDependOn(volatile_read));
  EXPECT_TRUE(volatile_read.MayDependOn(any_write));
  EXPECT_FALSE(any_write.MayDependOn(volatile_read));
}

TEST(SideEffectsTest, SameWidthTypes) {
  // Type I/F.
  testWriteAndReadDependence(
      SideEffects::FieldWriteOfType(Primitive::kPrimInt, /* is_volatile */ false),
      SideEffects::FieldReadOfType(Primitive::kPrimFloat, /* is_volatile */ false));
  testWriteAndReadDependence(
      SideEffects::ArrayWriteOfType(Primitive::kPrimInt),
      SideEffects::ArrayReadOfType(Primitive::kPrimFloat));
  // Type L/D.
  testWriteAndReadDependence(
      SideEffects::FieldWriteOfType(Primitive::kPrimLong, /* is_volatile */ false),
      SideEffects::FieldReadOfType(Primitive::kPrimDouble, /* is_volatile */ false));
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
    s = s.Union(SideEffects::FieldWriteOfType(type, /* is_volatile */ false));
    s = s.Union(SideEffects::ArrayWriteOfType(type));
    s = s.Union(SideEffects::FieldReadOfType(type, /* is_volatile */ false));
    s = s.Union(SideEffects::ArrayReadOfType(type));
  }
  EXPECT_TRUE(s.DoesAllReadWrite());
}

TEST(SideEffectsTest, GC) {
  SideEffects can_trigger_gc = SideEffects::CanTriggerGC();
  SideEffects depends_on_gc = SideEffects::DependsOnGC();
  SideEffects all_changes = SideEffects::AllChanges();
  SideEffects all_dependencies = SideEffects::AllDependencies();

  EXPECT_TRUE(depends_on_gc.MayDependOn(can_trigger_gc));
  EXPECT_TRUE(depends_on_gc.Union(can_trigger_gc).MayDependOn(can_trigger_gc));
  EXPECT_FALSE(can_trigger_gc.MayDependOn(depends_on_gc));

  EXPECT_TRUE(depends_on_gc.MayDependOn(all_changes));
  EXPECT_TRUE(depends_on_gc.Union(can_trigger_gc).MayDependOn(all_changes));
  EXPECT_FALSE(can_trigger_gc.MayDependOn(all_changes));

  EXPECT_TRUE(all_changes.Includes(can_trigger_gc));
  EXPECT_FALSE(all_changes.Includes(depends_on_gc));
  EXPECT_TRUE(all_dependencies.Includes(depends_on_gc));
  EXPECT_FALSE(all_dependencies.Includes(can_trigger_gc));
}

TEST(SideEffectsTest, BitStrings) {
  EXPECT_STREQ(
      "|||||||",
      SideEffects::None().ToString().c_str());
  EXPECT_STREQ(
      "|GC|DFJISCBZL|DFJISCBZL|GC|DFJISCBZL|DFJISCBZL|",
      SideEffects::All().ToString().c_str());
  EXPECT_STREQ(
      "|||||DFJISCBZL|DFJISCBZL|",
      SideEffects::AllWrites().ToString().c_str());
  EXPECT_STREQ(
      "||DFJISCBZL|DFJISCBZL||||",
      SideEffects::AllReads().ToString().c_str());
  EXPECT_STREQ(
      "||||||L|",
      SideEffects::FieldWriteOfType(Primitive::kPrimNot, false).ToString().c_str());
  EXPECT_STREQ(
      "|||||Z||",
      SideEffects::ArrayWriteOfType(Primitive::kPrimBoolean).ToString().c_str());
  EXPECT_STREQ(
      "|||B||||",
      SideEffects::FieldReadOfType(Primitive::kPrimByte, false).ToString().c_str());
  EXPECT_STREQ(
      "||DJ|||||",  // note: DJ alias
      SideEffects::ArrayReadOfType(Primitive::kPrimDouble).ToString().c_str());
  SideEffects s = SideEffects::None();
  s = s.Union(SideEffects::FieldWriteOfType(Primitive::kPrimChar, /* is_volatile */ false));
  s = s.Union(SideEffects::FieldWriteOfType(Primitive::kPrimLong, /* is_volatile */ false));
  s = s.Union(SideEffects::ArrayWriteOfType(Primitive::kPrimShort));
  s = s.Union(SideEffects::FieldReadOfType(Primitive::kPrimInt, /* is_volatile */ false));
  s = s.Union(SideEffects::ArrayReadOfType(Primitive::kPrimFloat));
  s = s.Union(SideEffects::ArrayReadOfType(Primitive::kPrimDouble));
  EXPECT_STREQ(
      "||DFJI|FI||S|DJC|",   // note: DJ/FI alias.
      s.ToString().c_str());
}

}  // namespace art
