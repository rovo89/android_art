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

#ifndef ART_COMPILER_UTILS_ARM64_MANAGED_REGISTER_ARM64_H_
#define ART_COMPILER_UTILS_ARM64_MANAGED_REGISTER_ARM64_H_

#include "base/logging.h"
#include "constants_arm64.h"
#include "utils/managed_register.h"

namespace art {
namespace arm64 {

const int kNumberOfCoreRegIds = kNumberOfCoreRegisters;
const int kNumberOfWRegIds = kNumberOfWRegisters;
const int kNumberOfDRegIds = kNumberOfDRegisters;
const int kNumberOfSRegIds = kNumberOfSRegisters;

const int kNumberOfRegIds = kNumberOfCoreRegIds + kNumberOfWRegIds +
  kNumberOfDRegIds + kNumberOfSRegIds;

// Register ids map:
//  [0..X[  core registers 64bit (enum Register)
//  [X..W[  core registers 32bit (enum WRegister)
//  [W..D[  double precision VFP registers (enum DRegister)
//  [D..S[  single precision VFP registers (enum SRegister)
//
// where:
//  X = kNumberOfCoreRegIds
//  W = X + kNumberOfWRegIds
//  D = W + kNumberOfDRegIds
//  S = D + kNumberOfSRegIds
//
// An instance of class 'ManagedRegister' represents a single Arm64
// register. A register can be one of the following:
//  * core register 64bit context (enum Register)
//  * core register 32bit context (enum WRegister)
//  * VFP double precision register (enum DRegister)
//  * VFP single precision register (enum SRegister)
//
// There is a one to one mapping between ManagedRegister and register id.

class Arm64ManagedRegister : public ManagedRegister {
 public:
  Register AsCoreRegister() const {
    CHECK(IsCoreRegister());
    return static_cast<Register>(id_);
  }

  WRegister AsWRegister() const {
    CHECK(IsWRegister());
    return static_cast<WRegister>(id_ - kNumberOfCoreRegIds);
  }

  DRegister AsDRegister() const {
    CHECK(IsDRegister());
    return static_cast<DRegister>(id_ - kNumberOfCoreRegIds - kNumberOfWRegIds);
  }

  SRegister AsSRegister() const {
    CHECK(IsSRegister());
    return static_cast<SRegister>(id_ - kNumberOfCoreRegIds - kNumberOfWRegIds -
                                  kNumberOfDRegIds);
  }

  WRegister AsOverlappingCoreRegisterLow() const {
    CHECK(IsValidManagedRegister());
    if (IsZeroRegister()) return W31;
    return static_cast<WRegister>(AsCoreRegister());
  }

  // FIXME: Find better naming.
  Register AsOverlappingWRegisterCore() const {
    CHECK(IsValidManagedRegister());
    return static_cast<Register>(AsWRegister());
  }

  SRegister AsOverlappingDRegisterLow() const {
    CHECK(IsValidManagedRegister());
    return static_cast<SRegister>(AsDRegister());
  }

  // FIXME: Find better naming.
  DRegister AsOverlappingSRegisterD() const {
    CHECK(IsValidManagedRegister());
    return static_cast<DRegister>(AsSRegister());
  }

  bool IsCoreRegister() const {
    CHECK(IsValidManagedRegister());
    return (0 <= id_) && (id_ < kNumberOfCoreRegIds);
  }

  bool IsWRegister() const {
    CHECK(IsValidManagedRegister());
    const int test = id_ - kNumberOfCoreRegIds;
    return (0 <= test) && (test < kNumberOfWRegIds);
  }

  bool IsDRegister() const {
    CHECK(IsValidManagedRegister());
    const int test = id_ - (kNumberOfCoreRegIds + kNumberOfWRegIds);
    return (0 <= test) && (test < kNumberOfDRegIds);
  }

  bool IsSRegister() const {
    CHECK(IsValidManagedRegister());
    const int test = id_ - (kNumberOfCoreRegIds + kNumberOfWRegIds +
                            kNumberOfDRegIds);
    return (0 <= test) && (test < kNumberOfSRegIds);
  }

  bool IsGPRegister() const {
    return IsCoreRegister() || IsWRegister();
  }

  bool IsFPRegister() const {
    return IsDRegister() || IsSRegister();
  }

  bool IsSameType(Arm64ManagedRegister test) const {
    CHECK(IsValidManagedRegister() && test.IsValidManagedRegister());
    return
      (IsCoreRegister() && test.IsCoreRegister()) ||
      (IsWRegister() && test.IsWRegister()) ||
      (IsDRegister() && test.IsDRegister()) ||
      (IsSRegister() && test.IsSRegister());
  }

  // Returns true if the two managed-registers ('this' and 'other') overlap.
  // Either managed-register may be the NoRegister. If both are the NoRegister
  // then false is returned.
  bool Overlaps(const Arm64ManagedRegister& other) const;

  void Print(std::ostream& os) const;

  static Arm64ManagedRegister FromCoreRegister(Register r) {
    CHECK_NE(r, kNoRegister);
    return FromRegId(r);
  }

  static Arm64ManagedRegister FromWRegister(WRegister r) {
    CHECK_NE(r, kNoWRegister);
    return FromRegId(r + kNumberOfCoreRegIds);
  }

  static Arm64ManagedRegister FromDRegister(DRegister r) {
    CHECK_NE(r, kNoDRegister);
    return FromRegId(r + (kNumberOfCoreRegIds + kNumberOfWRegIds));
  }

  static Arm64ManagedRegister FromSRegister(SRegister r) {
    CHECK_NE(r, kNoSRegister);
    return FromRegId(r + (kNumberOfCoreRegIds + kNumberOfWRegIds +
                          kNumberOfDRegIds));
  }

  // Returns the X register overlapping W register r.
  static Arm64ManagedRegister FromWRegisterCore(WRegister r) {
    CHECK_NE(r, kNoWRegister);
    return FromRegId(r);
  }

  // Return the D register overlapping S register r.
  static Arm64ManagedRegister FromSRegisterD(SRegister r) {
    CHECK_NE(r, kNoSRegister);
    return FromRegId(r + (kNumberOfCoreRegIds + kNumberOfWRegIds));
  }

 private:
  bool IsValidManagedRegister() const {
    return (0 <= id_) && (id_ < kNumberOfRegIds);
  }

  bool IsStackPointer() const {
    return IsCoreRegister() && (id_ == SP);
  }

  bool IsZeroRegister() const {
    return IsCoreRegister() && (id_ == XZR);
  }

  int RegId() const {
    CHECK(!IsNoRegister());
    return id_;
  }

  int RegNo() const;
  int RegIdLow() const;
  int RegIdHigh() const;

  friend class ManagedRegister;

  explicit Arm64ManagedRegister(int reg_id) : ManagedRegister(reg_id) {}

  static Arm64ManagedRegister FromRegId(int reg_id) {
    Arm64ManagedRegister reg(reg_id);
    CHECK(reg.IsValidManagedRegister());
    return reg;
  }
};

std::ostream& operator<<(std::ostream& os, const Arm64ManagedRegister& reg);

}  // namespace arm64

inline arm64::Arm64ManagedRegister ManagedRegister::AsArm64() const {
  arm64::Arm64ManagedRegister reg(id_);
  CHECK(reg.IsNoRegister() || reg.IsValidManagedRegister());
  return reg;
}

}  // namespace art

#endif  // ART_COMPILER_UTILS_ARM64_MANAGED_REGISTER_ARM64_H_
