// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/globals.h"
#include "src/managed_register_arm.h"
#include "gtest/gtest.h"

namespace art {

TEST(ManagedRegister, NoRegister) {
  ManagedRegister reg = ManagedRegister::NoRegister();
  EXPECT_TRUE(reg.IsNoRegister());
  EXPECT_TRUE(!reg.Overlaps(reg));
}

TEST(ManagedRegister, CoreRegister) {
  ManagedRegister reg = ManagedRegister::FromCoreRegister(R0);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(R0, reg.AsCoreRegister());

  reg = ManagedRegister::FromCoreRegister(R1);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(R1, reg.AsCoreRegister());

  reg = ManagedRegister::FromCoreRegister(R8);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(R8, reg.AsCoreRegister());

  reg = ManagedRegister::FromCoreRegister(R15);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(R15, reg.AsCoreRegister());
}


TEST(ManagedRegister, SRegister) {
  ManagedRegister reg = ManagedRegister::FromSRegister(S0);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(S0, reg.AsSRegister());

  reg = ManagedRegister::FromSRegister(S1);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(S1, reg.AsSRegister());

  reg = ManagedRegister::FromSRegister(S3);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(S3, reg.AsSRegister());

  reg = ManagedRegister::FromSRegister(S15);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(S15, reg.AsSRegister());

  reg = ManagedRegister::FromSRegister(S30);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(S30, reg.AsSRegister());

  reg = ManagedRegister::FromSRegister(S31);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(S31, reg.AsSRegister());
}


TEST(ManagedRegister, DRegister) {
  ManagedRegister reg = ManagedRegister::FromDRegister(D0);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(D0, reg.AsDRegister());
  EXPECT_EQ(S0, reg.AsOverlappingDRegisterLow());
  EXPECT_EQ(S1, reg.AsOverlappingDRegisterHigh());
  EXPECT_TRUE(reg.Equals(ManagedRegister::FromSRegisterPair(S0)));

  reg = ManagedRegister::FromDRegister(D1);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(D1, reg.AsDRegister());
  EXPECT_EQ(S2, reg.AsOverlappingDRegisterLow());
  EXPECT_EQ(S3, reg.AsOverlappingDRegisterHigh());
  EXPECT_TRUE(reg.Equals(ManagedRegister::FromSRegisterPair(S2)));

  reg = ManagedRegister::FromDRegister(D6);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(D6, reg.AsDRegister());
  EXPECT_EQ(S12, reg.AsOverlappingDRegisterLow());
  EXPECT_EQ(S13, reg.AsOverlappingDRegisterHigh());
  EXPECT_TRUE(reg.Equals(ManagedRegister::FromSRegisterPair(S12)));

  reg = ManagedRegister::FromDRegister(D14);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(D14, reg.AsDRegister());
  EXPECT_EQ(S28, reg.AsOverlappingDRegisterLow());
  EXPECT_EQ(S29, reg.AsOverlappingDRegisterHigh());
  EXPECT_TRUE(reg.Equals(ManagedRegister::FromSRegisterPair(S28)));

  reg = ManagedRegister::FromDRegister(D15);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(D15, reg.AsDRegister());
  EXPECT_EQ(S30, reg.AsOverlappingDRegisterLow());
  EXPECT_EQ(S31, reg.AsOverlappingDRegisterHigh());
  EXPECT_TRUE(reg.Equals(ManagedRegister::FromSRegisterPair(S30)));

#ifdef VFPv3_D32
  reg = ManagedRegister::FromDRegister(D16);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(D16, reg.AsDRegister());

  reg = ManagedRegister::FromDRegister(D18);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(D18, reg.AsDRegister());

  reg = ManagedRegister::FromDRegister(D30);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(D30, reg.AsDRegister());

  reg = ManagedRegister::FromDRegister(D31);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(D31, reg.AsDRegister());
#endif  // VFPv3_D32
}


TEST(ManagedRegister, Pair) {
  ManagedRegister reg = ManagedRegister::FromRegisterPair(R0_R1);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(R0_R1, reg.AsRegisterPair());
  EXPECT_EQ(R0, reg.AsRegisterPairLow());
  EXPECT_EQ(R1, reg.AsRegisterPairHigh());
  EXPECT_TRUE(reg.Equals(ManagedRegister::FromCoreRegisterPair(R0)));

  reg = ManagedRegister::FromRegisterPair(R2_R3);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(R2_R3, reg.AsRegisterPair());
  EXPECT_EQ(R2, reg.AsRegisterPairLow());
  EXPECT_EQ(R3, reg.AsRegisterPairHigh());
  EXPECT_TRUE(reg.Equals(ManagedRegister::FromCoreRegisterPair(R2)));

  reg = ManagedRegister::FromRegisterPair(R4_R5);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(R4_R5, reg.AsRegisterPair());
  EXPECT_EQ(R4, reg.AsRegisterPairLow());
  EXPECT_EQ(R5, reg.AsRegisterPairHigh());
  EXPECT_TRUE(reg.Equals(ManagedRegister::FromCoreRegisterPair(R4)));

  reg = ManagedRegister::FromRegisterPair(R6_R7);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsOverlappingDRegister());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(R6_R7, reg.AsRegisterPair());
  EXPECT_EQ(R6, reg.AsRegisterPairLow());
  EXPECT_EQ(R7, reg.AsRegisterPairHigh());
  EXPECT_TRUE(reg.Equals(ManagedRegister::FromCoreRegisterPair(R6)));
}


TEST(ManagedRegister, Equals) {
  ManagedRegister no_reg = ManagedRegister::NoRegister();
  EXPECT_TRUE(no_reg.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!no_reg.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!no_reg.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!no_reg.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!no_reg.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!no_reg.Equals(ManagedRegister::FromRegisterPair(R0_R1)));

  ManagedRegister reg_R0 = ManagedRegister::FromCoreRegister(R0);
  EXPECT_TRUE(!reg_R0.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(reg_R0.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_R0.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_R0.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_R0.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_R0.Equals(ManagedRegister::FromRegisterPair(R0_R1)));

  ManagedRegister reg_R1 = ManagedRegister::FromCoreRegister(R1);
  EXPECT_TRUE(!reg_R1.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_R1.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(reg_R1.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_R1.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_R1.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_R1.Equals(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg_R1.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_R1.Equals(ManagedRegister::FromRegisterPair(R0_R1)));

  ManagedRegister reg_R8 = ManagedRegister::FromCoreRegister(R8);
  EXPECT_TRUE(!reg_R8.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_R8.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(reg_R8.Equals(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg_R8.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_R8.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_R8.Equals(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg_R8.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_R8.Equals(ManagedRegister::FromRegisterPair(R0_R1)));

  ManagedRegister reg_S0 = ManagedRegister::FromSRegister(S0);
  EXPECT_TRUE(!reg_S0.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_S0.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_S0.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(reg_S0.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_S0.Equals(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg_S0.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_S0.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_S0.Equals(ManagedRegister::FromRegisterPair(R0_R1)));

  ManagedRegister reg_S1 = ManagedRegister::FromSRegister(S1);
  EXPECT_TRUE(!reg_S1.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_S1.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_S1.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_S1.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(reg_S1.Equals(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg_S1.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_S1.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_S1.Equals(ManagedRegister::FromRegisterPair(R0_R1)));

  ManagedRegister reg_S31 = ManagedRegister::FromSRegister(S31);
  EXPECT_TRUE(!reg_S31.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_S31.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_S31.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_S31.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(reg_S31.Equals(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg_S31.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_S31.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_S31.Equals(ManagedRegister::FromRegisterPair(R0_R1)));

  ManagedRegister reg_D0 = ManagedRegister::FromDRegister(D0);
  EXPECT_TRUE(!reg_D0.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_D0.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_D0.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_D0.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_D0.Equals(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(reg_D0.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_D0.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_D0.Equals(ManagedRegister::FromRegisterPair(R0_R1)));

  ManagedRegister reg_D15 = ManagedRegister::FromDRegister(D15);
  EXPECT_TRUE(!reg_D15.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_D15.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_D15.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_D15.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_D15.Equals(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg_D15.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_D15.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(reg_D15.Equals(ManagedRegister::FromDRegister(D15)));
  EXPECT_TRUE(!reg_D15.Equals(ManagedRegister::FromRegisterPair(R0_R1)));

#ifdef VFPv3_D32
  ManagedRegister reg_D16 = ManagedRegister::FromDRegister(D16);
  EXPECT_TRUE(!reg_D16.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_D16.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_D16.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_D16.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_D16.Equals(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg_D16.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_D16.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_D16.Equals(ManagedRegister::FromDRegister(D15)));
  EXPECT_TRUE(reg_D16.Equals(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg_D16.Equals(ManagedRegister::FromRegisterPair(R0_R1)));

  ManagedRegister reg_D30 = ManagedRegister::FromDRegister(D30);
  EXPECT_TRUE(!reg_D30.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_D30.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_D30.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_D30.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_D30.Equals(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg_D30.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_D30.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_D30.Equals(ManagedRegister::FromDRegister(D15)));
  EXPECT_TRUE(!reg_D30.Equals(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(reg_D30.Equals(ManagedRegister::FromDRegister(D30)));
  EXPECT_TRUE(!reg_D30.Equals(ManagedRegister::FromRegisterPair(R0_R1)));

  ManagedRegister reg_D31 = ManagedRegister::FromDRegister(D30);
  EXPECT_TRUE(!reg_D31.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_D31.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_D31.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_D31.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_D31.Equals(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg_D31.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_D31.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_D31.Equals(ManagedRegister::FromDRegister(D15)));
  EXPECT_TRUE(!reg_D31.Equals(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg_D31.Equals(ManagedRegister::FromDRegister(D30)));
  EXPECT_TRUE(reg_D31.Equals(ManagedRegister::FromDRegister(D31)));
  EXPECT_TRUE(!reg_D31.Equals(ManagedRegister::FromRegisterPair(R0_R1)));
#endif  // VFPv3_D32

  ManagedRegister reg_R0R1 = ManagedRegister::FromRegisterPair(R0_R1);
  EXPECT_TRUE(!reg_R0R1.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_R0R1.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_R0R1.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_R0R1.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_R0R1.Equals(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg_R0R1.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_R0R1.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_R0R1.Equals(ManagedRegister::FromDRegister(D15)));
  EXPECT_TRUE(reg_R0R1.Equals(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg_R0R1.Equals(ManagedRegister::FromRegisterPair(R2_R3)));

  ManagedRegister reg_R4R5 = ManagedRegister::FromRegisterPair(R4_R5);
  EXPECT_TRUE(!reg_R4R5.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_R4R5.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_R4R5.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_R4R5.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_R4R5.Equals(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg_R4R5.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_R4R5.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_R4R5.Equals(ManagedRegister::FromDRegister(D15)));
  EXPECT_TRUE(!reg_R4R5.Equals(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(reg_R4R5.Equals(ManagedRegister::FromRegisterPair(R4_R5)));
  EXPECT_TRUE(!reg_R4R5.Equals(ManagedRegister::FromRegisterPair(R6_R7)));

  ManagedRegister reg_R6R7 = ManagedRegister::FromRegisterPair(R6_R7);
  EXPECT_TRUE(!reg_R6R7.Equals(ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_R6R7.Equals(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg_R6R7.Equals(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg_R6R7.Equals(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_R6R7.Equals(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg_R6R7.Equals(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_R6R7.Equals(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_R6R7.Equals(ManagedRegister::FromDRegister(D15)));
  EXPECT_TRUE(!reg_R6R7.Equals(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg_R6R7.Equals(ManagedRegister::FromRegisterPair(R4_R5)));
  EXPECT_TRUE(reg_R6R7.Equals(ManagedRegister::FromRegisterPair(R6_R7)));
}


TEST(ManagedRegister, Overlaps) {
  ManagedRegister reg = ManagedRegister::FromCoreRegister(R0);
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

  reg = ManagedRegister::FromCoreRegister(R1);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

  reg = ManagedRegister::FromCoreRegister(R7);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

  reg = ManagedRegister::FromSRegister(S0);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

  reg = ManagedRegister::FromSRegister(S1);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

  reg = ManagedRegister::FromSRegister(S15);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

  reg = ManagedRegister::FromSRegister(S31);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

  reg = ManagedRegister::FromDRegister(D0);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

  reg = ManagedRegister::FromDRegister(D7);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

  reg = ManagedRegister::FromDRegister(D15);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

#ifdef VFPv3_D32
  reg = ManagedRegister::FromDRegister(D16);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

  reg = ManagedRegister::FromDRegister(D31);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromDRegister(D31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));
#endif  // VFPv3_D32

  reg = ManagedRegister::FromRegisterPair(R0_R1);
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));

  reg = ManagedRegister::FromRegisterPair(R4_R5);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCoreRegister(R8)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D15)));
#ifdef VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromDRegister(D31)));
#endif  // VFPv3_D32
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(R0_R1)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(R4_R5)));
}

}  // namespace art
