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

#include "instruction_set.h"

#include "base/stringprintf.h"
#include "common_runtime_test.h"

namespace art {

class InstructionSetTest : public CommonRuntimeTest {};

TEST_F(InstructionSetTest, GetInstructionSetFromString) {
  EXPECT_EQ(kArm, GetInstructionSetFromString("arm"));
  EXPECT_EQ(kArm64, GetInstructionSetFromString("arm64"));
  EXPECT_EQ(kX86, GetInstructionSetFromString("x86"));
  EXPECT_EQ(kX86_64, GetInstructionSetFromString("x86_64"));
  EXPECT_EQ(kMips, GetInstructionSetFromString("mips"));
  EXPECT_EQ(kNone, GetInstructionSetFromString("none"));
  EXPECT_EQ(kNone, GetInstructionSetFromString("random-string"));
}

TEST_F(InstructionSetTest, GetInstructionSetString) {
  EXPECT_STREQ("arm", GetInstructionSetString(kArm));
  EXPECT_STREQ("arm", GetInstructionSetString(kThumb2));
  EXPECT_STREQ("arm64", GetInstructionSetString(kArm64));
  EXPECT_STREQ("x86", GetInstructionSetString(kX86));
  EXPECT_STREQ("x86_64", GetInstructionSetString(kX86_64));
  EXPECT_STREQ("mips", GetInstructionSetString(kMips));
  EXPECT_STREQ("none", GetInstructionSetString(kNone));
}

TEST_F(InstructionSetTest, TestRoundTrip) {
  EXPECT_EQ(kRuntimeISA, GetInstructionSetFromString(GetInstructionSetString(kRuntimeISA)));
}

TEST_F(InstructionSetTest, PointerSize) {
  EXPECT_EQ(sizeof(void*), GetInstructionSetPointerSize(kRuntimeISA));
}

TEST_F(InstructionSetTest, X86Features) {
  // Build features for a 32-bit x86 atom processor.
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> x86_features(
      InstructionSetFeatures::FromVariant(kX86, "atom", &error_msg));
  ASSERT_TRUE(x86_features.get() != nullptr) << error_msg;
  EXPECT_EQ(x86_features->GetInstructionSet(), kX86);
  EXPECT_TRUE(x86_features->Equals(x86_features.get()));
  EXPECT_STREQ("none", x86_features->GetFeatureString().c_str());
  EXPECT_EQ(x86_features->AsBitmap(), 0U);

  // Build features for a 32-bit x86 default processor.
  std::unique_ptr<const InstructionSetFeatures> x86_default_features(
      InstructionSetFeatures::FromFeatureString(kX86, "default", &error_msg));
  ASSERT_TRUE(x86_default_features.get() != nullptr) << error_msg;
  EXPECT_EQ(x86_default_features->GetInstructionSet(), kX86);
  EXPECT_TRUE(x86_default_features->Equals(x86_default_features.get()));
  EXPECT_STREQ("none", x86_default_features->GetFeatureString().c_str());
  EXPECT_EQ(x86_default_features->AsBitmap(), 0U);

  // Build features for a 64-bit x86-64 atom processor.
  std::unique_ptr<const InstructionSetFeatures> x86_64_features(
      InstructionSetFeatures::FromVariant(kX86_64, "atom", &error_msg));
  ASSERT_TRUE(x86_64_features.get() != nullptr) << error_msg;
  EXPECT_EQ(x86_64_features->GetInstructionSet(), kX86_64);
  EXPECT_TRUE(x86_64_features->Equals(x86_64_features.get()));
  EXPECT_STREQ("none", x86_64_features->GetFeatureString().c_str());
  EXPECT_EQ(x86_64_features->AsBitmap(), 0U);

  EXPECT_FALSE(x86_64_features->Equals(x86_features.get()));
  EXPECT_FALSE(x86_64_features->Equals(x86_default_features.get()));
  EXPECT_TRUE(x86_features->Equals(x86_default_features.get()));
}

TEST_F(InstructionSetTest, ArmFeaturesFromVariant) {
  // Build features for a 32-bit ARM krait processor.
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> krait_features(
      InstructionSetFeatures::FromVariant(kArm, "krait", &error_msg));
  ASSERT_TRUE(krait_features.get() != nullptr) << error_msg;

  ASSERT_EQ(krait_features->GetInstructionSet(), kArm);
  EXPECT_TRUE(krait_features->Equals(krait_features.get()));
  EXPECT_TRUE(krait_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_TRUE(krait_features->AsArmInstructionSetFeatures()->HasLpae());
  EXPECT_STREQ("div,lpae", krait_features->GetFeatureString().c_str());
  EXPECT_EQ(krait_features->AsBitmap(), 3U);

  // Build features for a 32-bit ARM denver processor.
  std::unique_ptr<const InstructionSetFeatures> denver_features(
      InstructionSetFeatures::FromVariant(kArm, "denver", &error_msg));
  ASSERT_TRUE(denver_features.get() != nullptr) << error_msg;

  EXPECT_TRUE(denver_features->Equals(denver_features.get()));
  EXPECT_TRUE(denver_features->Equals(krait_features.get()));
  EXPECT_TRUE(krait_features->Equals(denver_features.get()));
  EXPECT_TRUE(denver_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_TRUE(denver_features->AsArmInstructionSetFeatures()->HasLpae());
  EXPECT_STREQ("div,lpae", denver_features->GetFeatureString().c_str());
  EXPECT_EQ(denver_features->AsBitmap(), 3U);

  // Build features for a 32-bit ARMv7 processor.
  std::unique_ptr<const InstructionSetFeatures> arm7_features(
      InstructionSetFeatures::FromVariant(kArm, "arm7", &error_msg));
  ASSERT_TRUE(arm7_features.get() != nullptr) << error_msg;

  EXPECT_TRUE(arm7_features->Equals(arm7_features.get()));
  EXPECT_FALSE(arm7_features->Equals(krait_features.get()));
  EXPECT_FALSE(krait_features->Equals(arm7_features.get()));
  EXPECT_FALSE(arm7_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_FALSE(arm7_features->AsArmInstructionSetFeatures()->HasLpae());
  EXPECT_STREQ("none", arm7_features->GetFeatureString().c_str());
  EXPECT_EQ(arm7_features->AsBitmap(), 0U);

  // ARM6 is not a supported architecture variant.
  std::unique_ptr<const InstructionSetFeatures> arm6_features(
      InstructionSetFeatures::FromVariant(kArm, "arm6", &error_msg));
  EXPECT_TRUE(arm6_features.get() == nullptr);
  EXPECT_NE(error_msg.size(), 0U);
}

TEST_F(InstructionSetTest, ArmFeaturesFromString) {
  // Build features for a 32-bit ARM with LPAE and div processor.
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> krait_features(
      InstructionSetFeatures::FromFeatureString(kArm, "lpae,div", &error_msg));
  ASSERT_TRUE(krait_features.get() != nullptr) << error_msg;

  ASSERT_EQ(krait_features->GetInstructionSet(), kArm);
  EXPECT_TRUE(krait_features->Equals(krait_features.get()));
  EXPECT_TRUE(krait_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_TRUE(krait_features->AsArmInstructionSetFeatures()->HasLpae());
  EXPECT_STREQ("div,lpae", krait_features->GetFeatureString().c_str());
  EXPECT_EQ(krait_features->AsBitmap(), 3U);

  // Build features for a 32-bit ARM processor with LPAE and div flipped.
  std::unique_ptr<const InstructionSetFeatures> denver_features(
      InstructionSetFeatures::FromFeatureString(kArm, "div,lpae", &error_msg));
  ASSERT_TRUE(denver_features.get() != nullptr) << error_msg;

  EXPECT_TRUE(denver_features->Equals(denver_features.get()));
  EXPECT_TRUE(denver_features->Equals(krait_features.get()));
  EXPECT_TRUE(krait_features->Equals(denver_features.get()));
  EXPECT_TRUE(denver_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_TRUE(denver_features->AsArmInstructionSetFeatures()->HasLpae());
  EXPECT_STREQ("div,lpae", denver_features->GetFeatureString().c_str());
  EXPECT_EQ(denver_features->AsBitmap(), 3U);

  // Build features for a 32-bit default ARM processor.
  std::unique_ptr<const InstructionSetFeatures> arm7_features(
      InstructionSetFeatures::FromFeatureString(kArm, "default", &error_msg));
  ASSERT_TRUE(arm7_features.get() != nullptr) << error_msg;

  EXPECT_TRUE(arm7_features->Equals(arm7_features.get()));
  EXPECT_FALSE(arm7_features->Equals(krait_features.get()));
  EXPECT_FALSE(krait_features->Equals(arm7_features.get()));
  EXPECT_FALSE(arm7_features->AsArmInstructionSetFeatures()->HasDivideInstruction());
  EXPECT_FALSE(arm7_features->AsArmInstructionSetFeatures()->HasLpae());
  EXPECT_STREQ("none", arm7_features->GetFeatureString().c_str());
  EXPECT_EQ(arm7_features->AsBitmap(), 0U);
}

#ifdef HAVE_ANDROID_OS
#include "cutils/properties.h"

TEST_F(InstructionSetTest, FeaturesFromSystemPropertyVariant) {
  // Take the default set of instruction features from the build.
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features(
      InstructionSetFeatures::FromCppDefines());

  // Read the features property.
  std::string key = StringPrintf("dalvik.vm.isa.%s.variant", GetInstructionSetString(kRuntimeISA));
  char dex2oat_isa_variant[PROPERTY_VALUE_MAX];
  if (property_get(key.c_str(), dex2oat_isa_variant, nullptr) > 0) {
    // Use features from property to build InstructionSetFeatures and check against build's
    // features.
    std::string error_msg;
    std::unique_ptr<const InstructionSetFeatures> property_features(
        InstructionSetFeatures::FromVariant(kRuntimeISA, dex2oat_isa_variant, &error_msg));
    ASSERT_TRUE(property_features.get() != nullptr) << error_msg;

    EXPECT_TRUE(property_features->Equals(instruction_set_features.get()))
      << "System property features: " << *property_features.get()
      << "\nFeatures from build: " << *instruction_set_features.get();
  }
}

TEST_F(InstructionSetTest, FeaturesFromSystemPropertyString) {
  // Take the default set of instruction features from the build.
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features(
      InstructionSetFeatures::FromCppDefines());

  // Read the features property.
  std::string key = StringPrintf("dalvik.vm.isa.%s.features", GetInstructionSetString(kRuntimeISA));
  char dex2oat_isa_features[PROPERTY_VALUE_MAX];
  if (property_get(key.c_str(), dex2oat_isa_features, nullptr) > 0) {
    // Use features from property to build InstructionSetFeatures and check against build's
    // features.
    std::string error_msg;
    std::unique_ptr<const InstructionSetFeatures> property_features(
        InstructionSetFeatures::FromFeatureString(kRuntimeISA, dex2oat_isa_features, &error_msg));
    ASSERT_TRUE(property_features.get() != nullptr) << error_msg;

    EXPECT_TRUE(property_features->Equals(instruction_set_features.get()))
      << "System property features: " << *property_features.get()
      << "\nFeatures from build: " << *instruction_set_features.get();
  }
}
#endif

TEST_F(InstructionSetTest, FeaturesFromCpuInfo) {
  // Take the default set of instruction features from the build.
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features(
      InstructionSetFeatures::FromCppDefines());

  // Check we get the same instruction set features using /proc/cpuinfo.
  std::unique_ptr<const InstructionSetFeatures> cpuinfo_features(
      InstructionSetFeatures::FromCpuInfo());
  EXPECT_TRUE(cpuinfo_features->Equals(instruction_set_features.get()))
      << "CPU Info features: " << *cpuinfo_features.get()
      << "\nFeatures from build: " << *instruction_set_features.get();
}

TEST_F(InstructionSetTest, FeaturesFromHwcap) {
  // Take the default set of instruction features from the build.
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features(
      InstructionSetFeatures::FromCppDefines());

  // Check we get the same instruction set features using AT_HWCAP.
  std::unique_ptr<const InstructionSetFeatures> hwcap_features(
      InstructionSetFeatures::FromHwcap());
  EXPECT_TRUE(hwcap_features->Equals(instruction_set_features.get()))
      << "Hwcap features: " << *hwcap_features.get()
      << "\nFeatures from build: " << *instruction_set_features.get();
}


TEST_F(InstructionSetTest, FeaturesFromAssembly) {
  // Take the default set of instruction features from the build.
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features(
      InstructionSetFeatures::FromCppDefines());

  // Check we get the same instruction set features using assembly tests.
  std::unique_ptr<const InstructionSetFeatures> assembly_features(
      InstructionSetFeatures::FromAssembly());
  EXPECT_TRUE(assembly_features->Equals(instruction_set_features.get()))
      << "Assembly features: " << *assembly_features.get()
      << "\nFeatures from build: " << *instruction_set_features.get();
}

}  // namespace art
