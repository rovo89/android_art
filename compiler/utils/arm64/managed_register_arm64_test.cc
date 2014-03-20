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

#include "globals.h"
#include "managed_register_arm64.h"
#include "gtest/gtest.h"

namespace art {
namespace arm64 {

TEST(Arm64ManagedRegister, NoRegister) {
  Arm64ManagedRegister reg = ManagedRegister::NoRegister().AsArm64();
  EXPECT_TRUE(reg.IsNoRegister());
  EXPECT_TRUE(!reg.Overlaps(reg));
}

// X Register test.
TEST(Arm64ManagedRegister, CoreRegister) {
  Arm64ManagedRegister reg = Arm64ManagedRegister::FromCoreRegister(X0);
  Arm64ManagedRegister wreg = Arm64ManagedRegister::FromWRegister(W0);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(wreg));
  EXPECT_EQ(X0, reg.AsCoreRegister());

  reg = Arm64ManagedRegister::FromCoreRegister(X1);
  wreg = Arm64ManagedRegister::FromWRegister(W1);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(wreg));
  EXPECT_EQ(X1, reg.AsCoreRegister());

  reg = Arm64ManagedRegister::FromCoreRegister(X7);
  wreg = Arm64ManagedRegister::FromWRegister(W7);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(wreg));
  EXPECT_EQ(X7, reg.AsCoreRegister());

  reg = Arm64ManagedRegister::FromCoreRegister(X15);
  wreg = Arm64ManagedRegister::FromWRegister(W15);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(wreg));
  EXPECT_EQ(X15, reg.AsCoreRegister());

  reg = Arm64ManagedRegister::FromCoreRegister(X19);
  wreg = Arm64ManagedRegister::FromWRegister(W19);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(wreg));
  EXPECT_EQ(X19, reg.AsCoreRegister());

  reg = Arm64ManagedRegister::FromCoreRegister(X16);
  wreg = Arm64ManagedRegister::FromWRegister(W16);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(wreg));
  EXPECT_EQ(IP0, reg.AsCoreRegister());

  reg = Arm64ManagedRegister::FromCoreRegister(SP);
  wreg = Arm64ManagedRegister::FromWRegister(WZR);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(wreg));
  EXPECT_EQ(SP, reg.AsCoreRegister());
}

// W register test.
TEST(Arm64ManagedRegister, WRegister) {
  Arm64ManagedRegister reg = Arm64ManagedRegister::FromWRegister(W0);
  Arm64ManagedRegister xreg = Arm64ManagedRegister::FromCoreRegister(X0);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(xreg));
  EXPECT_EQ(W0, reg.AsWRegister());

  reg = Arm64ManagedRegister::FromWRegister(W5);
  xreg = Arm64ManagedRegister::FromCoreRegister(X5);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(xreg));
  EXPECT_EQ(W5, reg.AsWRegister());

  reg = Arm64ManagedRegister::FromWRegister(W6);
  xreg = Arm64ManagedRegister::FromCoreRegister(X6);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(xreg));
  EXPECT_EQ(W6, reg.AsWRegister());

  reg = Arm64ManagedRegister::FromWRegister(W18);
  xreg = Arm64ManagedRegister::FromCoreRegister(X18);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(xreg));
  EXPECT_EQ(W18, reg.AsWRegister());

  reg = Arm64ManagedRegister::FromWRegister(W29);
  xreg = Arm64ManagedRegister::FromCoreRegister(FP);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(xreg));
  EXPECT_EQ(W29, reg.AsWRegister());

  reg = Arm64ManagedRegister::FromWRegister(WZR);
  xreg = Arm64ManagedRegister::FromCoreRegister(SP);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(reg.IsWRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(xreg));
  EXPECT_EQ(W31, reg.AsWRegister());
}

// D Register test.
TEST(Arm64ManagedRegister, DRegister) {
  Arm64ManagedRegister reg = Arm64ManagedRegister::FromDRegister(D0);
  Arm64ManagedRegister sreg = Arm64ManagedRegister::FromSRegister(S0);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(sreg));
  EXPECT_EQ(D0, reg.AsDRegister());
  EXPECT_EQ(S0, reg.AsOverlappingDRegisterLow());
  EXPECT_TRUE(reg.Equals(Arm64ManagedRegister::FromDRegister(D0)));

  reg = Arm64ManagedRegister::FromDRegister(D1);
  sreg = Arm64ManagedRegister::FromSRegister(S1);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(sreg));
  EXPECT_EQ(D1, reg.AsDRegister());
  EXPECT_EQ(S1, reg.AsOverlappingDRegisterLow());
  EXPECT_TRUE(reg.Equals(Arm64ManagedRegister::FromDRegister(D1)));

  reg = Arm64ManagedRegister::FromDRegister(D20);
  sreg = Arm64ManagedRegister::FromSRegister(S20);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(sreg));
  EXPECT_EQ(D20, reg.AsDRegister());
  EXPECT_EQ(S20, reg.AsOverlappingDRegisterLow());
  EXPECT_TRUE(reg.Equals(Arm64ManagedRegister::FromDRegister(D20)));

  reg = Arm64ManagedRegister::FromDRegister(D31);
  sreg = Arm64ManagedRegister::FromSRegister(S31);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(reg.IsDRegister());
  EXPECT_TRUE(!reg.IsSRegister());
  EXPECT_TRUE(reg.Overlaps(sreg));
  EXPECT_EQ(D31, reg.AsDRegister());
  EXPECT_EQ(S31, reg.AsOverlappingDRegisterLow());
  EXPECT_TRUE(reg.Equals(Arm64ManagedRegister::FromDRegister(D31)));
}

// S Register test.
TEST(Arm64ManagedRegister, SRegister) {
  Arm64ManagedRegister reg = Arm64ManagedRegister::FromSRegister(S0);
  Arm64ManagedRegister dreg = Arm64ManagedRegister::FromDRegister(D0);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(reg.Overlaps(dreg));
  EXPECT_EQ(S0, reg.AsSRegister());
  EXPECT_EQ(D0, reg.AsOverlappingSRegisterD());
  EXPECT_TRUE(reg.Equals(Arm64ManagedRegister::FromSRegister(S0)));

  reg = Arm64ManagedRegister::FromSRegister(S5);
  dreg = Arm64ManagedRegister::FromDRegister(D5);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(reg.Overlaps(dreg));
  EXPECT_EQ(S5, reg.AsSRegister());
  EXPECT_EQ(D5, reg.AsOverlappingSRegisterD());
  EXPECT_TRUE(reg.Equals(Arm64ManagedRegister::FromSRegister(S5)));

  reg = Arm64ManagedRegister::FromSRegister(S7);
  dreg = Arm64ManagedRegister::FromDRegister(D7);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(reg.Overlaps(dreg));
  EXPECT_EQ(S7, reg.AsSRegister());
  EXPECT_EQ(D7, reg.AsOverlappingSRegisterD());
  EXPECT_TRUE(reg.Equals(Arm64ManagedRegister::FromSRegister(S7)));

  reg = Arm64ManagedRegister::FromSRegister(S31);
  dreg = Arm64ManagedRegister::FromDRegister(D31);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCoreRegister());
  EXPECT_TRUE(!reg.IsWRegister());
  EXPECT_TRUE(reg.IsSRegister());
  EXPECT_TRUE(!reg.IsDRegister());
  EXPECT_TRUE(reg.Overlaps(dreg));
  EXPECT_EQ(S31, reg.AsSRegister());
  EXPECT_EQ(D31, reg.AsOverlappingSRegisterD());
  EXPECT_TRUE(reg.Equals(Arm64ManagedRegister::FromSRegister(S31)));
}

TEST(Arm64ManagedRegister, Equals) {
  ManagedRegister no_reg = ManagedRegister::NoRegister();
  EXPECT_TRUE(no_reg.Equals(Arm64ManagedRegister::NoRegister()));
  EXPECT_TRUE(!no_reg.Equals(Arm64ManagedRegister::FromCoreRegister(X0)));
  EXPECT_TRUE(!no_reg.Equals(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!no_reg.Equals(Arm64ManagedRegister::FromWRegister(W0)));
  EXPECT_TRUE(!no_reg.Equals(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(!no_reg.Equals(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!no_reg.Equals(Arm64ManagedRegister::FromSRegister(S0)));

  Arm64ManagedRegister reg_X0 = Arm64ManagedRegister::FromCoreRegister(X0);
  EXPECT_TRUE(!reg_X0.Equals(Arm64ManagedRegister::NoRegister()));
  EXPECT_TRUE(reg_X0.Equals(Arm64ManagedRegister::FromCoreRegister(X0)));
  EXPECT_TRUE(!reg_X0.Equals(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg_X0.Equals(Arm64ManagedRegister::FromWRegister(W0)));
  EXPECT_TRUE(!reg_X0.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_X0.Equals(Arm64ManagedRegister::FromDRegister(D0)));

  Arm64ManagedRegister reg_X1 = Arm64ManagedRegister::FromCoreRegister(X1);
  EXPECT_TRUE(!reg_X1.Equals(Arm64ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_X1.Equals(Arm64ManagedRegister::FromCoreRegister(X0)));
  EXPECT_TRUE(reg_X1.Equals(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg_X1.Equals(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(!reg_X1.Equals(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_X1.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_X1.Equals(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_X1.Equals(Arm64ManagedRegister::FromSRegister(S1)));

  Arm64ManagedRegister reg_X31 = Arm64ManagedRegister::FromCoreRegister(X31);
  EXPECT_TRUE(!reg_X31.Equals(Arm64ManagedRegister::NoRegister()));
  // TODO: Fix the infrastructure, then re-enable.
  // EXPECT_TRUE(!reg_X31.Equals(Arm64ManagedRegister::FromCoreRegister(SP)));
  // EXPECT_TRUE(reg_X31.Equals(Arm64ManagedRegister::FromCoreRegister(XZR)));
  EXPECT_TRUE(!reg_X31.Equals(Arm64ManagedRegister::FromWRegister(W31)));
  EXPECT_TRUE(!reg_X31.Equals(Arm64ManagedRegister::FromWRegister(WZR)));
  EXPECT_TRUE(!reg_X31.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_X31.Equals(Arm64ManagedRegister::FromDRegister(D0)));

  Arm64ManagedRegister reg_SP = Arm64ManagedRegister::FromCoreRegister(SP);
  EXPECT_TRUE(!reg_SP.Equals(Arm64ManagedRegister::NoRegister()));
  // TODO: We expect these to pass - SP has a different semantic than X31/XZR.
  // EXPECT_TRUE(!reg_SP.Equals(Arm64ManagedRegister::FromCoreRegister(X31)));
  EXPECT_TRUE(!reg_SP.Equals(Arm64ManagedRegister::FromCoreRegister(XZR)));
  EXPECT_TRUE(!reg_SP.Equals(Arm64ManagedRegister::FromWRegister(W31)));
  EXPECT_TRUE(!reg_SP.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_SP.Equals(Arm64ManagedRegister::FromDRegister(D0)));

  Arm64ManagedRegister reg_W8 = Arm64ManagedRegister::FromWRegister(W8);
  EXPECT_TRUE(!reg_W8.Equals(Arm64ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_W8.Equals(Arm64ManagedRegister::FromCoreRegister(X0)));
  EXPECT_TRUE(!reg_W8.Equals(Arm64ManagedRegister::FromCoreRegister(X8)));
  EXPECT_TRUE(reg_W8.Equals(Arm64ManagedRegister::FromWRegister(W8)));
  EXPECT_TRUE(!reg_W8.Equals(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_W8.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_W8.Equals(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_W8.Equals(Arm64ManagedRegister::FromSRegister(S1)));

  Arm64ManagedRegister reg_W12 = Arm64ManagedRegister::FromWRegister(W12);
  EXPECT_TRUE(!reg_W12.Equals(Arm64ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_W12.Equals(Arm64ManagedRegister::FromCoreRegister(X0)));
  EXPECT_TRUE(!reg_W12.Equals(Arm64ManagedRegister::FromCoreRegister(X8)));
  EXPECT_TRUE(reg_W12.Equals(Arm64ManagedRegister::FromWRegister(W12)));
  EXPECT_TRUE(!reg_W12.Equals(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_W12.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_W12.Equals(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg_W12.Equals(Arm64ManagedRegister::FromSRegister(S1)));

  Arm64ManagedRegister reg_S0 = Arm64ManagedRegister::FromSRegister(S0);
  EXPECT_TRUE(!reg_S0.Equals(Arm64ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_S0.Equals(Arm64ManagedRegister::FromCoreRegister(X0)));
  EXPECT_TRUE(!reg_S0.Equals(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg_S0.Equals(Arm64ManagedRegister::FromWRegister(W0)));
  EXPECT_TRUE(reg_S0.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_S0.Equals(Arm64ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg_S0.Equals(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_S0.Equals(Arm64ManagedRegister::FromDRegister(D1)));

  Arm64ManagedRegister reg_S1 = Arm64ManagedRegister::FromSRegister(S1);
  EXPECT_TRUE(!reg_S1.Equals(Arm64ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_S1.Equals(Arm64ManagedRegister::FromCoreRegister(X0)));
  EXPECT_TRUE(!reg_S1.Equals(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg_S1.Equals(Arm64ManagedRegister::FromWRegister(W0)));
  EXPECT_TRUE(!reg_S1.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(reg_S1.Equals(Arm64ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg_S1.Equals(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_S1.Equals(Arm64ManagedRegister::FromDRegister(D1)));

  Arm64ManagedRegister reg_S31 = Arm64ManagedRegister::FromSRegister(S31);
  EXPECT_TRUE(!reg_S31.Equals(Arm64ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_S31.Equals(Arm64ManagedRegister::FromCoreRegister(X0)));
  EXPECT_TRUE(!reg_S31.Equals(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg_S31.Equals(Arm64ManagedRegister::FromWRegister(W0)));
  EXPECT_TRUE(!reg_S31.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(reg_S31.Equals(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg_S31.Equals(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_S31.Equals(Arm64ManagedRegister::FromDRegister(D1)));

  Arm64ManagedRegister reg_D0 = Arm64ManagedRegister::FromDRegister(D0);
  EXPECT_TRUE(!reg_D0.Equals(Arm64ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_D0.Equals(Arm64ManagedRegister::FromCoreRegister(X0)));
  EXPECT_TRUE(!reg_D0.Equals(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(!reg_D0.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_D0.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_D0.Equals(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(reg_D0.Equals(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_D0.Equals(Arm64ManagedRegister::FromDRegister(D1)));

  Arm64ManagedRegister reg_D15 = Arm64ManagedRegister::FromDRegister(D15);
  EXPECT_TRUE(!reg_D15.Equals(Arm64ManagedRegister::NoRegister()));
  EXPECT_TRUE(!reg_D15.Equals(Arm64ManagedRegister::FromCoreRegister(X0)));
  EXPECT_TRUE(!reg_D15.Equals(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg_D15.Equals(Arm64ManagedRegister::FromWRegister(W0)));
  EXPECT_TRUE(!reg_D15.Equals(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg_D15.Equals(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg_D15.Equals(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg_D15.Equals(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(reg_D15.Equals(Arm64ManagedRegister::FromDRegister(D15)));
}

TEST(Arm64ManagedRegister, Overlaps) {
  Arm64ManagedRegister reg = Arm64ManagedRegister::FromCoreRegister(X0);
  Arm64ManagedRegister reg_o = Arm64ManagedRegister::FromWRegister(W0);
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(SP)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromWRegister(W0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W12)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(WZR)));
  EXPECT_EQ(X0, reg_o.AsOverlappingWRegisterCore());
  EXPECT_EQ(W0, reg.AsOverlappingCoreRegisterLow());
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D15)));

  reg = Arm64ManagedRegister::FromCoreRegister(X10);
  reg_o = Arm64ManagedRegister::FromWRegister(W10);
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X10)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(SP)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromWRegister(W10)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W12)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(WZR)));
  EXPECT_EQ(X10, reg_o.AsOverlappingWRegisterCore());
  EXPECT_EQ(W10, reg.AsOverlappingCoreRegisterLow());
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D15)));

  reg = Arm64ManagedRegister::FromCoreRegister(IP1);
  reg_o = Arm64ManagedRegister::FromWRegister(W17);
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X17)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(SP)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromWRegister(W17)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W12)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(WZR)));
  EXPECT_EQ(X17, reg_o.AsOverlappingWRegisterCore());
  EXPECT_EQ(W17, reg.AsOverlappingCoreRegisterLow());
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D15)));

  reg = Arm64ManagedRegister::FromCoreRegister(XZR);
  reg_o = Arm64ManagedRegister::FromWRegister(WZR);
  // TODO: Overlap not implemented, yet
  // EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X1)));
  // EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(SP)));
  // EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromWRegister(W31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W12)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W19)));
  EXPECT_EQ(X31, reg_o.AsOverlappingWRegisterCore());
  // TODO: XZR is not a core register right now.
  // EXPECT_EQ(W31, reg.AsOverlappingCoreRegisterLow());
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D15)));

  reg = Arm64ManagedRegister::FromCoreRegister(SP);
  reg_o = Arm64ManagedRegister::FromWRegister(WZR);
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X15)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromWRegister(WZR)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W12)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromWRegister(W31)));
  EXPECT_EQ(X31, reg_o.AsOverlappingWRegisterCore());
  EXPECT_EQ(W31, reg.AsOverlappingCoreRegisterLow());
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D15)));

  reg = Arm64ManagedRegister::FromWRegister(W1);
  reg_o = Arm64ManagedRegister::FromCoreRegister(X1);
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(WZR)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W12)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W31)));
  EXPECT_EQ(W1, reg_o.AsOverlappingCoreRegisterLow());
  EXPECT_EQ(X1, reg.AsOverlappingWRegisterCore());
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D15)));

  reg = Arm64ManagedRegister::FromWRegister(W21);
  reg_o = Arm64ManagedRegister::FromCoreRegister(X21);
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromWRegister(W21)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X21)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(WZR)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W12)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W31)));
  EXPECT_EQ(W21, reg_o.AsOverlappingCoreRegisterLow());
  EXPECT_EQ(X21, reg.AsOverlappingWRegisterCore());
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D15)));


  reg = Arm64ManagedRegister::FromSRegister(S1);
  reg_o = Arm64ManagedRegister::FromDRegister(D1);
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(WZR)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W12)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W31)));
  EXPECT_EQ(S1, reg_o.AsOverlappingDRegisterLow());
  EXPECT_EQ(D1, reg.AsOverlappingSRegisterD());
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromSRegister(S1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S30)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D0)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromDRegister(D1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D7)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D15)));

  reg = Arm64ManagedRegister::FromSRegister(S15);
  reg_o = Arm64ManagedRegister::FromDRegister(D15);
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(WZR)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W12)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W31)));
  EXPECT_EQ(S15, reg_o.AsOverlappingDRegisterLow());
  EXPECT_EQ(D15, reg.AsOverlappingSRegisterD());
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S17)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S16)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromDRegister(D15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D17)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D20)));

  reg = Arm64ManagedRegister::FromDRegister(D15);
  reg_o = Arm64ManagedRegister::FromSRegister(S15);
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromCoreRegister(X15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(WZR)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W1)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W12)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromWRegister(W31)));
  EXPECT_EQ(S15, reg.AsOverlappingDRegisterLow());
  EXPECT_EQ(D15, reg_o.AsOverlappingSRegisterD());
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S0)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromSRegister(S15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S17)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S16)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromSRegister(S31)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D16)));
  EXPECT_TRUE(reg.Overlaps(Arm64ManagedRegister::FromDRegister(D15)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D2)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D17)));
  EXPECT_TRUE(!reg.Overlaps(Arm64ManagedRegister::FromDRegister(D20)));
}

}  // namespace arm64
}  // namespace art
