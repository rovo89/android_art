// Copyright 2011 Google Inc. All Rights Reserved.

#include "globals.h"
#include "managed_register.h"
#include "gtest/gtest.h"

namespace art {

TEST(ManagedRegister, NoRegister) {
  ManagedRegister reg = ManagedRegister::NoRegister();
  EXPECT_TRUE(reg.IsNoRegister());
  EXPECT_TRUE(!reg.Overlaps(reg));
}

TEST(ManagedRegister, CpuRegister) {
  ManagedRegister reg = ManagedRegister::FromCpuRegister(EAX);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(EAX, reg.AsCpuRegister());

  reg = ManagedRegister::FromCpuRegister(EBX);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(EBX, reg.AsCpuRegister());

  reg = ManagedRegister::FromCpuRegister(ECX);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(ECX, reg.AsCpuRegister());

  reg = ManagedRegister::FromCpuRegister(EDI);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(EDI, reg.AsCpuRegister());
}

TEST(ManagedRegister, XmmRegister) {
  ManagedRegister reg = ManagedRegister::FromXmmRegister(XMM0);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(XMM0, reg.AsXmmRegister());

  reg = ManagedRegister::FromXmmRegister(XMM1);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(XMM1, reg.AsXmmRegister());

  reg = ManagedRegister::FromXmmRegister(XMM7);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(XMM7, reg.AsXmmRegister());
}

TEST(ManagedRegister, X87Register) {
  ManagedRegister reg = ManagedRegister::FromX87Register(ST0);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(reg.IsX87Register());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(ST0, reg.AsX87Register());

  reg = ManagedRegister::FromX87Register(ST1);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(reg.IsX87Register());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(ST1, reg.AsX87Register());

  reg = ManagedRegister::FromX87Register(ST7);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(reg.IsX87Register());
  EXPECT_TRUE(!reg.IsRegisterPair());
  EXPECT_EQ(ST7, reg.AsX87Register());
}

TEST(ManagedRegister, RegisterPair) {
  ManagedRegister reg = ManagedRegister::FromRegisterPair(EAX_EDX);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(EAX, reg.AsRegisterPairLow());
  EXPECT_EQ(EDX, reg.AsRegisterPairHigh());

  reg = ManagedRegister::FromRegisterPair(EAX_ECX);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(EAX, reg.AsRegisterPairLow());
  EXPECT_EQ(ECX, reg.AsRegisterPairHigh());

  reg = ManagedRegister::FromRegisterPair(EAX_EBX);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(EAX, reg.AsRegisterPairLow());
  EXPECT_EQ(EBX, reg.AsRegisterPairHigh());

  reg = ManagedRegister::FromRegisterPair(EAX_EDI);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(EAX, reg.AsRegisterPairLow());
  EXPECT_EQ(EDI, reg.AsRegisterPairHigh());

  reg = ManagedRegister::FromRegisterPair(EDX_ECX);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(EDX, reg.AsRegisterPairLow());
  EXPECT_EQ(ECX, reg.AsRegisterPairHigh());

  reg = ManagedRegister::FromRegisterPair(EDX_EBX);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(EDX, reg.AsRegisterPairLow());
  EXPECT_EQ(EBX, reg.AsRegisterPairHigh());

  reg = ManagedRegister::FromRegisterPair(EDX_EDI);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(EDX, reg.AsRegisterPairLow());
  EXPECT_EQ(EDI, reg.AsRegisterPairHigh());

  reg = ManagedRegister::FromRegisterPair(ECX_EBX);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(ECX, reg.AsRegisterPairLow());
  EXPECT_EQ(EBX, reg.AsRegisterPairHigh());

  reg = ManagedRegister::FromRegisterPair(ECX_EDI);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(ECX, reg.AsRegisterPairLow());
  EXPECT_EQ(EDI, reg.AsRegisterPairHigh());

  reg = ManagedRegister::FromRegisterPair(EBX_EDI);
  EXPECT_TRUE(!reg.IsNoRegister());
  EXPECT_TRUE(!reg.IsCpuRegister());
  EXPECT_TRUE(!reg.IsXmmRegister());
  EXPECT_TRUE(!reg.IsX87Register());
  EXPECT_TRUE(reg.IsRegisterPair());
  EXPECT_EQ(EBX, reg.AsRegisterPairLow());
  EXPECT_EQ(EDI, reg.AsRegisterPairHigh());
}

TEST(ManagedRegister, Equals) {
  ManagedRegister reg_eax = ManagedRegister::FromCpuRegister(EAX);
  EXPECT_TRUE(reg_eax.Equals(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(!reg_eax.Equals(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(!reg_eax.Equals(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(!reg_eax.Equals(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg_eax.Equals(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(!reg_eax.Equals(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg_eax.Equals(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(!reg_eax.Equals(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(!reg_eax.Equals(ManagedRegister::FromRegisterPair(EBX_EDI)));

  ManagedRegister reg_xmm0 = ManagedRegister::FromXmmRegister(XMM0);
  EXPECT_TRUE(!reg_xmm0.Equals(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(!reg_xmm0.Equals(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(!reg_xmm0.Equals(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(reg_xmm0.Equals(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg_xmm0.Equals(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(!reg_xmm0.Equals(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg_xmm0.Equals(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(!reg_xmm0.Equals(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(!reg_xmm0.Equals(ManagedRegister::FromRegisterPair(EBX_EDI)));

  ManagedRegister reg_st0 = ManagedRegister::FromX87Register(ST0);
  EXPECT_TRUE(!reg_st0.Equals(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(!reg_st0.Equals(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(!reg_st0.Equals(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(!reg_st0.Equals(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg_st0.Equals(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(reg_st0.Equals(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg_st0.Equals(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(!reg_st0.Equals(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(!reg_st0.Equals(ManagedRegister::FromRegisterPair(EBX_EDI)));

  ManagedRegister reg_pair = ManagedRegister::FromRegisterPair(EAX_EDX);
  EXPECT_TRUE(!reg_pair.Equals(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(!reg_pair.Equals(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(!reg_pair.Equals(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(!reg_pair.Equals(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg_pair.Equals(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(!reg_pair.Equals(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg_pair.Equals(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(reg_pair.Equals(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(!reg_pair.Equals(ManagedRegister::FromRegisterPair(EBX_EDI)));
}

TEST(ManagedRegister, Overlaps) {
  ManagedRegister reg = ManagedRegister::FromCpuRegister(EAX);
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(EBX_EDI)));

  reg = ManagedRegister::FromCpuRegister(EDX);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(EBX_EDI)));

  reg = ManagedRegister::FromCpuRegister(EDI);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(EBX_EDI)));

  reg = ManagedRegister::FromCpuRegister(EBX);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(EBX_EDI)));

  reg = ManagedRegister::FromXmmRegister(XMM0);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(EBX_EDI)));

  reg = ManagedRegister::FromX87Register(ST0);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(EBX_EDI)));

  reg = ManagedRegister::FromRegisterPair(EAX_EDX);
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(EDX_ECX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(EBX_EDI)));

  reg = ManagedRegister::FromRegisterPair(EBX_EDI);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(EBX_EDI)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(EDX_EBX)));

  reg = ManagedRegister::FromRegisterPair(EDX_ECX);
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EAX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EBX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromCpuRegister(EDI)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromXmmRegister(XMM7)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST0)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromX87Register(ST7)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(EAX_EDX)));
  EXPECT_TRUE(!reg.Overlaps(ManagedRegister::FromRegisterPair(EBX_EDI)));
  EXPECT_TRUE(reg.Overlaps(ManagedRegister::FromRegisterPair(EDX_EBX)));
}

}  // namespace art
