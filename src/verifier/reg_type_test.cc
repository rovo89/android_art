/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "reg_type.h"

#include "reg_type_cache.h"

#include "common_test.h"

namespace art {
namespace verifier {

class RegTypeTest : public CommonTest {};

TEST_F(RegTypeTest, Primitives) {
  ScopedObjectAccess soa(Thread::Current());
  RegTypeCache cache;

  const RegType& bool_reg_type = cache.Boolean();
  EXPECT_FALSE(bool_reg_type.IsUndefined());
  EXPECT_FALSE(bool_reg_type.IsConflict());
  EXPECT_FALSE(bool_reg_type.IsZero());
  EXPECT_FALSE(bool_reg_type.IsOne());
  EXPECT_FALSE(bool_reg_type.IsLongConstant());
  EXPECT_TRUE(bool_reg_type.IsBoolean());
  EXPECT_FALSE(bool_reg_type.IsByte());
  EXPECT_FALSE(bool_reg_type.IsChar());
  EXPECT_FALSE(bool_reg_type.IsShort());
  EXPECT_FALSE(bool_reg_type.IsInteger());
  EXPECT_FALSE(bool_reg_type.IsLong());
  EXPECT_FALSE(bool_reg_type.IsFloat());
  EXPECT_FALSE(bool_reg_type.IsDouble());
  EXPECT_FALSE(bool_reg_type.IsReference());
  EXPECT_FALSE(bool_reg_type.IsLowHalf());
  EXPECT_FALSE(bool_reg_type.IsHighHalf());
  EXPECT_FALSE(bool_reg_type.IsLongOrDoubleTypes());
  EXPECT_FALSE(bool_reg_type.IsReferenceTypes());
  EXPECT_TRUE(bool_reg_type.IsCategory1Types());
  EXPECT_FALSE(bool_reg_type.IsCategory2Types());
  EXPECT_TRUE(bool_reg_type.IsBooleanTypes());
  EXPECT_TRUE(bool_reg_type.IsByteTypes());
  EXPECT_TRUE(bool_reg_type.IsShortTypes());
  EXPECT_TRUE(bool_reg_type.IsCharTypes());
  EXPECT_TRUE(bool_reg_type.IsIntegralTypes());
  EXPECT_FALSE(bool_reg_type.IsFloatTypes());
  EXPECT_FALSE(bool_reg_type.IsLongTypes());
  EXPECT_FALSE(bool_reg_type.IsDoubleTypes());
  EXPECT_TRUE(bool_reg_type.IsArrayIndexTypes());

  const RegType& byte_reg_type = cache.Byte();
  EXPECT_FALSE(byte_reg_type.IsUndefined());
  EXPECT_FALSE(byte_reg_type.IsConflict());
  EXPECT_FALSE(byte_reg_type.IsZero());
  EXPECT_FALSE(byte_reg_type.IsOne());
  EXPECT_FALSE(byte_reg_type.IsLongConstant());
  EXPECT_FALSE(byte_reg_type.IsBoolean());
  EXPECT_TRUE(byte_reg_type.IsByte());
  EXPECT_FALSE(byte_reg_type.IsChar());
  EXPECT_FALSE(byte_reg_type.IsShort());
  EXPECT_FALSE(byte_reg_type.IsInteger());
  EXPECT_FALSE(byte_reg_type.IsLong());
  EXPECT_FALSE(byte_reg_type.IsFloat());
  EXPECT_FALSE(byte_reg_type.IsDouble());
  EXPECT_FALSE(byte_reg_type.IsReference());
  EXPECT_FALSE(byte_reg_type.IsLowHalf());
  EXPECT_FALSE(byte_reg_type.IsHighHalf());
  EXPECT_FALSE(byte_reg_type.IsLongOrDoubleTypes());
  EXPECT_FALSE(byte_reg_type.IsReferenceTypes());
  EXPECT_TRUE(byte_reg_type.IsCategory1Types());
  EXPECT_FALSE(byte_reg_type.IsCategory2Types());
  EXPECT_FALSE(byte_reg_type.IsBooleanTypes());
  EXPECT_TRUE(byte_reg_type.IsByteTypes());
  EXPECT_TRUE(byte_reg_type.IsShortTypes());
  EXPECT_FALSE(byte_reg_type.IsCharTypes());
  EXPECT_TRUE(byte_reg_type.IsIntegralTypes());
  EXPECT_FALSE(byte_reg_type.IsFloatTypes());
  EXPECT_FALSE(byte_reg_type.IsLongTypes());
  EXPECT_FALSE(byte_reg_type.IsDoubleTypes());
  EXPECT_TRUE(byte_reg_type.IsArrayIndexTypes());

  const RegType& char_reg_type = cache.Char();
  EXPECT_FALSE(char_reg_type.IsUndefined());
  EXPECT_FALSE(char_reg_type.IsConflict());
  EXPECT_FALSE(char_reg_type.IsZero());
  EXPECT_FALSE(char_reg_type.IsOne());
  EXPECT_FALSE(char_reg_type.IsLongConstant());
  EXPECT_FALSE(char_reg_type.IsBoolean());
  EXPECT_FALSE(char_reg_type.IsByte());
  EXPECT_TRUE(char_reg_type.IsChar());
  EXPECT_FALSE(char_reg_type.IsShort());
  EXPECT_FALSE(char_reg_type.IsInteger());
  EXPECT_FALSE(char_reg_type.IsLong());
  EXPECT_FALSE(char_reg_type.IsFloat());
  EXPECT_FALSE(char_reg_type.IsDouble());
  EXPECT_FALSE(char_reg_type.IsReference());
  EXPECT_FALSE(char_reg_type.IsLowHalf());
  EXPECT_FALSE(char_reg_type.IsHighHalf());
  EXPECT_FALSE(char_reg_type.IsLongOrDoubleTypes());
  EXPECT_FALSE(char_reg_type.IsReferenceTypes());
  EXPECT_TRUE(char_reg_type.IsCategory1Types());
  EXPECT_FALSE(char_reg_type.IsCategory2Types());
  EXPECT_FALSE(char_reg_type.IsBooleanTypes());
  EXPECT_FALSE(char_reg_type.IsByteTypes());
  EXPECT_FALSE(char_reg_type.IsShortTypes());
  EXPECT_TRUE(char_reg_type.IsCharTypes());
  EXPECT_TRUE(char_reg_type.IsIntegralTypes());
  EXPECT_FALSE(char_reg_type.IsFloatTypes());
  EXPECT_FALSE(char_reg_type.IsLongTypes());
  EXPECT_FALSE(char_reg_type.IsDoubleTypes());
  EXPECT_TRUE(char_reg_type.IsArrayIndexTypes());

  const RegType& short_reg_type = cache.Short();
  EXPECT_FALSE(short_reg_type.IsUndefined());
  EXPECT_FALSE(short_reg_type.IsConflict());
  EXPECT_FALSE(short_reg_type.IsZero());
  EXPECT_FALSE(short_reg_type.IsOne());
  EXPECT_FALSE(short_reg_type.IsLongConstant());
  EXPECT_FALSE(short_reg_type.IsBoolean());
  EXPECT_FALSE(short_reg_type.IsByte());
  EXPECT_FALSE(short_reg_type.IsChar());
  EXPECT_TRUE(short_reg_type.IsShort());
  EXPECT_FALSE(short_reg_type.IsInteger());
  EXPECT_FALSE(short_reg_type.IsLong());
  EXPECT_FALSE(short_reg_type.IsFloat());
  EXPECT_FALSE(short_reg_type.IsDouble());
  EXPECT_FALSE(short_reg_type.IsReference());
  EXPECT_FALSE(short_reg_type.IsLowHalf());
  EXPECT_FALSE(short_reg_type.IsHighHalf());
  EXPECT_FALSE(short_reg_type.IsLongOrDoubleTypes());
  EXPECT_FALSE(short_reg_type.IsReferenceTypes());
  EXPECT_TRUE(short_reg_type.IsCategory1Types());
  EXPECT_FALSE(short_reg_type.IsCategory2Types());
  EXPECT_FALSE(short_reg_type.IsBooleanTypes());
  EXPECT_FALSE(short_reg_type.IsByteTypes());
  EXPECT_TRUE(short_reg_type.IsShortTypes());
  EXPECT_FALSE(short_reg_type.IsCharTypes());
  EXPECT_TRUE(short_reg_type.IsIntegralTypes());
  EXPECT_FALSE(short_reg_type.IsFloatTypes());
  EXPECT_FALSE(short_reg_type.IsLongTypes());
  EXPECT_FALSE(short_reg_type.IsDoubleTypes());
  EXPECT_TRUE(short_reg_type.IsArrayIndexTypes());

  const RegType& int_reg_type = cache.Integer();
  EXPECT_FALSE(int_reg_type.IsUndefined());
  EXPECT_FALSE(int_reg_type.IsConflict());
  EXPECT_FALSE(int_reg_type.IsZero());
  EXPECT_FALSE(int_reg_type.IsOne());
  EXPECT_FALSE(int_reg_type.IsLongConstant());
  EXPECT_FALSE(int_reg_type.IsBoolean());
  EXPECT_FALSE(int_reg_type.IsByte());
  EXPECT_FALSE(int_reg_type.IsChar());
  EXPECT_FALSE(int_reg_type.IsShort());
  EXPECT_TRUE(int_reg_type.IsInteger());
  EXPECT_FALSE(int_reg_type.IsLong());
  EXPECT_FALSE(int_reg_type.IsFloat());
  EXPECT_FALSE(int_reg_type.IsDouble());
  EXPECT_FALSE(int_reg_type.IsReference());
  EXPECT_FALSE(int_reg_type.IsLowHalf());
  EXPECT_FALSE(int_reg_type.IsHighHalf());
  EXPECT_FALSE(int_reg_type.IsLongOrDoubleTypes());
  EXPECT_FALSE(int_reg_type.IsReferenceTypes());
  EXPECT_TRUE(int_reg_type.IsCategory1Types());
  EXPECT_FALSE(int_reg_type.IsCategory2Types());
  EXPECT_FALSE(int_reg_type.IsBooleanTypes());
  EXPECT_FALSE(int_reg_type.IsByteTypes());
  EXPECT_FALSE(int_reg_type.IsShortTypes());
  EXPECT_FALSE(int_reg_type.IsCharTypes());
  EXPECT_TRUE(int_reg_type.IsIntegralTypes());
  EXPECT_FALSE(int_reg_type.IsFloatTypes());
  EXPECT_FALSE(int_reg_type.IsLongTypes());
  EXPECT_FALSE(int_reg_type.IsDoubleTypes());
  EXPECT_TRUE(int_reg_type.IsArrayIndexTypes());

  const RegType& long_reg_type = cache.LongLo();
  EXPECT_FALSE(long_reg_type.IsUndefined());
  EXPECT_FALSE(long_reg_type.IsConflict());
  EXPECT_FALSE(long_reg_type.IsZero());
  EXPECT_FALSE(long_reg_type.IsOne());
  EXPECT_FALSE(long_reg_type.IsLongConstant());
  EXPECT_FALSE(long_reg_type.IsBoolean());
  EXPECT_FALSE(long_reg_type.IsByte());
  EXPECT_FALSE(long_reg_type.IsChar());
  EXPECT_FALSE(long_reg_type.IsShort());
  EXPECT_FALSE(long_reg_type.IsInteger());
  EXPECT_TRUE(long_reg_type.IsLong());
  EXPECT_FALSE(long_reg_type.IsFloat());
  EXPECT_FALSE(long_reg_type.IsDouble());
  EXPECT_FALSE(long_reg_type.IsReference());
  EXPECT_TRUE(long_reg_type.IsLowHalf());
  EXPECT_FALSE(long_reg_type.IsHighHalf());
  EXPECT_TRUE(long_reg_type.IsLongOrDoubleTypes());
  EXPECT_FALSE(long_reg_type.IsReferenceTypes());
  EXPECT_FALSE(long_reg_type.IsCategory1Types());
  EXPECT_TRUE(long_reg_type.IsCategory2Types());
  EXPECT_FALSE(long_reg_type.IsBooleanTypes());
  EXPECT_FALSE(long_reg_type.IsByteTypes());
  EXPECT_FALSE(long_reg_type.IsShortTypes());
  EXPECT_FALSE(long_reg_type.IsCharTypes());
  EXPECT_FALSE(long_reg_type.IsIntegralTypes());
  EXPECT_FALSE(long_reg_type.IsFloatTypes());
  EXPECT_TRUE(long_reg_type.IsLongTypes());
  EXPECT_FALSE(long_reg_type.IsDoubleTypes());
  EXPECT_FALSE(long_reg_type.IsArrayIndexTypes());

  const RegType& float_reg_type = cache.Float();
  EXPECT_FALSE(float_reg_type.IsUndefined());
  EXPECT_FALSE(float_reg_type.IsConflict());
  EXPECT_FALSE(float_reg_type.IsZero());
  EXPECT_FALSE(float_reg_type.IsOne());
  EXPECT_FALSE(float_reg_type.IsLongConstant());
  EXPECT_FALSE(float_reg_type.IsBoolean());
  EXPECT_FALSE(float_reg_type.IsByte());
  EXPECT_FALSE(float_reg_type.IsChar());
  EXPECT_FALSE(float_reg_type.IsShort());
  EXPECT_FALSE(float_reg_type.IsInteger());
  EXPECT_FALSE(float_reg_type.IsLong());
  EXPECT_TRUE(float_reg_type.IsFloat());
  EXPECT_FALSE(float_reg_type.IsDouble());
  EXPECT_FALSE(float_reg_type.IsReference());
  EXPECT_FALSE(float_reg_type.IsLowHalf());
  EXPECT_FALSE(float_reg_type.IsHighHalf());
  EXPECT_FALSE(float_reg_type.IsLongOrDoubleTypes());
  EXPECT_FALSE(float_reg_type.IsReferenceTypes());
  EXPECT_TRUE(float_reg_type.IsCategory1Types());
  EXPECT_FALSE(float_reg_type.IsCategory2Types());
  EXPECT_FALSE(float_reg_type.IsBooleanTypes());
  EXPECT_FALSE(float_reg_type.IsByteTypes());
  EXPECT_FALSE(float_reg_type.IsShortTypes());
  EXPECT_FALSE(float_reg_type.IsCharTypes());
  EXPECT_FALSE(float_reg_type.IsIntegralTypes());
  EXPECT_TRUE(float_reg_type.IsFloatTypes());
  EXPECT_FALSE(float_reg_type.IsLongTypes());
  EXPECT_FALSE(float_reg_type.IsDoubleTypes());
  EXPECT_FALSE(float_reg_type.IsArrayIndexTypes());

  const RegType& double_reg_type = cache.DoubleLo();
  EXPECT_FALSE(double_reg_type.IsUndefined());
  EXPECT_FALSE(double_reg_type.IsConflict());
  EXPECT_FALSE(double_reg_type.IsZero());
  EXPECT_FALSE(double_reg_type.IsOne());
  EXPECT_FALSE(double_reg_type.IsLongConstant());
  EXPECT_FALSE(double_reg_type.IsBoolean());
  EXPECT_FALSE(double_reg_type.IsByte());
  EXPECT_FALSE(double_reg_type.IsChar());
  EXPECT_FALSE(double_reg_type.IsShort());
  EXPECT_FALSE(double_reg_type.IsInteger());
  EXPECT_FALSE(double_reg_type.IsLong());
  EXPECT_FALSE(double_reg_type.IsFloat());
  EXPECT_TRUE(double_reg_type.IsDouble());
  EXPECT_FALSE(double_reg_type.IsReference());
  EXPECT_TRUE(double_reg_type.IsLowHalf());
  EXPECT_FALSE(double_reg_type.IsHighHalf());
  EXPECT_TRUE(double_reg_type.IsLongOrDoubleTypes());
  EXPECT_FALSE(double_reg_type.IsReferenceTypes());
  EXPECT_FALSE(double_reg_type.IsCategory1Types());
  EXPECT_TRUE(double_reg_type.IsCategory2Types());
  EXPECT_FALSE(double_reg_type.IsBooleanTypes());
  EXPECT_FALSE(double_reg_type.IsByteTypes());
  EXPECT_FALSE(double_reg_type.IsShortTypes());
  EXPECT_FALSE(double_reg_type.IsCharTypes());
  EXPECT_FALSE(double_reg_type.IsIntegralTypes());
  EXPECT_FALSE(double_reg_type.IsFloatTypes());
  EXPECT_FALSE(double_reg_type.IsLongTypes());
  EXPECT_TRUE(double_reg_type.IsDoubleTypes());
  EXPECT_FALSE(double_reg_type.IsArrayIndexTypes());
}

// TODO: test reference RegType
// TODO: test constant RegType
// TODO: test VerifyAgainst
// TODO: test Merge
// TODO: test Equals
// TODO: test ClassJoin

}  // namespace verifier
}  // namespace art
