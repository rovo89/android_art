// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_MANAGED_REGISTER_ARM_H_
#define ART_SRC_MANAGED_REGISTER_ARM_H_

#include "constants.h"
#include "logging.h"

namespace art {

// Values for register pairs.
enum RegisterPair {
  R0_R1 = 0,
  R2_R3 = 1,
  R4_R5 = 2,
  R6_R7 = 3,
  kNumberOfRegisterPairs = 4,
  kNoRegisterPair = -1,
};

std::ostream& operator<<(std::ostream& os, const RegisterPair& reg);

const int kNumberOfCoreRegIds = kNumberOfCoreRegisters;
const int kNumberOfCoreAllocIds = kNumberOfCoreRegisters;

const int kNumberOfSRegIds = kNumberOfSRegisters;
const int kNumberOfSAllocIds = kNumberOfSRegisters;

const int kNumberOfDRegIds = kNumberOfDRegisters;
const int kNumberOfOverlappingDRegIds = kNumberOfOverlappingDRegisters;
const int kNumberOfDAllocIds = kNumberOfDRegIds - kNumberOfOverlappingDRegIds;

const int kNumberOfPairRegIds = kNumberOfRegisterPairs;

const int kNumberOfRegIds = kNumberOfCoreRegIds + kNumberOfSRegIds +
    kNumberOfDRegIds + kNumberOfPairRegIds;
const int kNumberOfAllocIds =
    kNumberOfCoreAllocIds + kNumberOfSAllocIds + kNumberOfDAllocIds;

// Register ids map:
//   [0..R[  core registers (enum Register)
//   [R..S[  single precision VFP registers (enum SRegister)
//   [S..D[  double precision VFP registers (enum DRegister)
//   [D..P[  core register pairs (enum RegisterPair)
// where
//   R = kNumberOfCoreRegIds
//   S = R + kNumberOfSRegIds
//   D = S + kNumberOfDRegIds
//   P = D + kNumberOfRegisterPairs

// Allocation ids map:
//   [0..R[  core registers (enum Register)
//   [R..S[  single precision VFP registers (enum SRegister)
//   [S..N[  non-overlapping double precision VFP registers (16-31 in enum
//           DRegister, VFPv3-D32 only)
// where
//   R = kNumberOfCoreAllocIds
//   S = R + kNumberOfSAllocIds
//   N = S + kNumberOfDAllocIds


// An instance of class 'ManagedRegister' represents a single ARM register or a
// pair of core ARM registers (enum RegisterPair). A single register is either a
// core register (enum Register), a VFP single precision register
// (enum SRegister), or a VFP double precision register (enum DRegister).
// 'ManagedRegister::NoRegister()' returns an invalid ManagedRegister.
// There is a one-to-one mapping between ManagedRegister and register id.
class ManagedRegister {
 public:
  // ManagedRegister is a value class. There exists no method to change the
  // internal state. We therefore allow a copy constructor and an
  // assignment-operator.
  ManagedRegister(const ManagedRegister& other) : id_(other.id_) { }

  ManagedRegister& operator=(const ManagedRegister& other) {
    id_ = other.id_;
    return *this;
  }

  Register AsCoreRegister() const {
    CHECK(IsCoreRegister());
    return static_cast<Register>(id_);
  }

  SRegister AsSRegister() const {
    CHECK(IsSRegister());
    return static_cast<SRegister>(id_ - kNumberOfCoreRegIds);
  }

  DRegister AsDRegister() const {
    CHECK(IsDRegister());
    return static_cast<DRegister>(id_ - kNumberOfCoreRegIds - kNumberOfSRegIds);
  }

  SRegister AsOverlappingDRegisterLow() const {
    CHECK(IsOverlappingDRegister());
    DRegister d_reg = AsDRegister();
    return static_cast<SRegister>(d_reg * 2);
  }

  SRegister AsOverlappingDRegisterHigh() const {
    CHECK(IsOverlappingDRegister());
    DRegister d_reg = AsDRegister();
    return static_cast<SRegister>(d_reg * 2 + 1);
  }

  RegisterPair AsRegisterPair() const {
    CHECK(IsRegisterPair());
    Register reg_low = AsRegisterPairLow();
    return static_cast<RegisterPair>(reg_low / 2);
  }

  Register AsRegisterPairLow() const {
    CHECK(IsRegisterPair());
    // Appropriate mapping of register ids allows to use AllocIdLow().
    return FromRegId(AllocIdLow()).AsCoreRegister();
  }

  Register AsRegisterPairHigh() const {
    CHECK(IsRegisterPair());
    // Appropriate mapping of register ids allows to use AllocIdHigh().
    return FromRegId(AllocIdHigh()).AsCoreRegister();
  }

  bool IsCoreRegister() const {
    CHECK(IsValidManagedRegister());
    return (0 <= id_) && (id_ < kNumberOfCoreRegIds);
  }

  bool IsSRegister() const {
    CHECK(IsValidManagedRegister());
    const int test = id_ - kNumberOfCoreRegIds;
    return (0 <= test) && (test < kNumberOfSRegIds);
  }

  bool IsDRegister() const {
    CHECK(IsValidManagedRegister());
    const int test = id_ - (kNumberOfCoreRegIds + kNumberOfSRegIds);
    return (0 <= test) && (test < kNumberOfDRegIds);
  }

  // Returns true if this DRegister overlaps SRegisters.
  bool IsOverlappingDRegister() const {
    CHECK(IsValidManagedRegister());
    const int test = id_ - (kNumberOfCoreRegIds + kNumberOfSRegIds);
    return (0 <= test) && (test < kNumberOfOverlappingDRegIds);
  }

  bool IsRegisterPair() const {
    CHECK(IsValidManagedRegister());
    const int test =
        id_ - (kNumberOfCoreRegIds + kNumberOfSRegIds + kNumberOfDRegIds);
    return (0 <= test) && (test < kNumberOfPairRegIds);
  }

  bool IsSameType(ManagedRegister test) const {
    CHECK(IsValidManagedRegister() && test.IsValidManagedRegister());
    return
      (IsCoreRegister() && test.IsCoreRegister()) ||
      (IsSRegister() && test.IsSRegister()) ||
      (IsDRegister() && test.IsDRegister()) ||
      (IsRegisterPair() && test.IsRegisterPair());
  }

  bool IsNoRegister() const {
    return id_ == kNoRegister;
  }

  // It is valid to invoke Equals on and with a NoRegister.
  bool Equals(const ManagedRegister& other) const {
    return id_ == other.id_;
  }

  // Returns true if the two managed-registers ('this' and 'other') overlap.
  // Either managed-register may be the NoRegister. If both are the NoRegister
  // then false is returned.
  bool Overlaps(const ManagedRegister& other) const;

  void Print(std::ostream& os) const;

  static ManagedRegister NoRegister() {
    return ManagedRegister();
  }

  static ManagedRegister FromCoreRegister(Register r) {
    CHECK_NE(r, kNoRegister);
    return FromRegId(r);
  }

  static ManagedRegister FromSRegister(SRegister r) {
    CHECK_NE(r, kNoSRegister);
    return FromRegId(r + kNumberOfCoreRegIds);
  }

  static ManagedRegister FromDRegister(DRegister r) {
    CHECK_NE(r, kNoDRegister);
    return FromRegId(r + (kNumberOfCoreRegIds + kNumberOfSRegIds));
  }

  static ManagedRegister FromRegisterPair(RegisterPair r) {
    CHECK_NE(r, kNoRegisterPair);
    return FromRegId(r + (kNumberOfCoreRegIds +
                          kNumberOfSRegIds + kNumberOfDRegIds));
  }

  // Return a RegisterPair consisting of Register r_low and r_low + 1.
  static ManagedRegister FromCoreRegisterPair(Register r_low) {
    CHECK_NE(r_low, kNoRegister);
    CHECK_EQ(0, (r_low % 2));
    const int r = r_low / 2;
    CHECK_LT(r, kNumberOfPairRegIds);
    return FromRegisterPair(static_cast<RegisterPair>(r));
  }

  // Return a DRegister overlapping SRegister r_low and r_low + 1.
  static ManagedRegister FromSRegisterPair(SRegister r_low) {
    CHECK_NE(r_low, kNoSRegister);
    CHECK_EQ(0, (r_low % 2));
    const int r = r_low / 2;
    CHECK_LT(r, kNumberOfOverlappingDRegIds);
    return FromDRegister(static_cast<DRegister>(r));
  }

 private:
  static const int kNoRegister = -1;

  ManagedRegister() : id_(kNoRegister) { }

  bool IsValidManagedRegister() const {
    return (0 <= id_) && (id_ < kNumberOfRegIds);
  }

  int RegId() const {
    CHECK(!IsNoRegister());
    return id_;
  }

  int AllocId() const {
    CHECK(IsValidManagedRegister() &&
           !IsOverlappingDRegister() && !IsRegisterPair());
    int r = id_;
    if ((kNumberOfDAllocIds > 0) && IsDRegister()) {  // VFPv3-D32 only.
      r -= kNumberOfOverlappingDRegIds;
    }
    CHECK_LT(r, kNumberOfAllocIds);
    return r;
  }

  int AllocIdLow() const;
  int AllocIdHigh() const;

  static ManagedRegister FromRegId(int reg_id) {
    ManagedRegister reg;
    reg.id_ = reg_id;
    CHECK(reg.IsValidManagedRegister());
    return reg;
  }

  int id_;
};

std::ostream& operator<<(std::ostream& os, const ManagedRegister& reg);

}  // namespace art

#endif  // ART_SRC_MANAGED_REGISTER_ARM_H_
