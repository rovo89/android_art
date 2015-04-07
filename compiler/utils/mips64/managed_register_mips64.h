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

#ifndef ART_COMPILER_UTILS_MIPS64_MANAGED_REGISTER_MIPS64_H_
#define ART_COMPILER_UTILS_MIPS64_MANAGED_REGISTER_MIPS64_H_

#include "constants_mips64.h"
#include "dwarf/register.h"
#include "utils/managed_register.h"

namespace art {
namespace mips64 {

const int kNumberOfGpuRegIds = kNumberOfGpuRegisters;
const int kNumberOfGpuAllocIds = kNumberOfGpuRegisters;

const int kNumberOfFpuRegIds = kNumberOfFpuRegisters;
const int kNumberOfFpuAllocIds = kNumberOfFpuRegisters;

const int kNumberOfRegIds = kNumberOfGpuRegIds + kNumberOfFpuRegIds;
const int kNumberOfAllocIds = kNumberOfGpuAllocIds + kNumberOfFpuAllocIds;

// An instance of class 'ManagedRegister' represents a single GPU register (enum
// Register) or a double precision FP register (enum FpuRegister)
// 'ManagedRegister::NoRegister()' provides an invalid register.
// There is a one-to-one mapping between ManagedRegister and register id.
class Mips64ManagedRegister : public ManagedRegister {
 public:
  GpuRegister AsGpuRegister() const {
    CHECK(IsGpuRegister());
    return static_cast<GpuRegister>(id_);
  }

  FpuRegister AsFpuRegister() const {
    CHECK(IsFpuRegister());
    return static_cast<FpuRegister>(id_ - kNumberOfGpuRegIds);
  }

  bool IsGpuRegister() const {
    CHECK(IsValidManagedRegister());
    return (0 <= id_) && (id_ < kNumberOfGpuRegIds);
  }

  bool IsFpuRegister() const {
    CHECK(IsValidManagedRegister());
    const int test = id_ - kNumberOfGpuRegIds;
    return (0 <= test) && (test < kNumberOfFpuRegIds);
  }

  void Print(std::ostream& os) const;

  // Returns true if the two managed-registers ('this' and 'other') overlap.
  // Either managed-register may be the NoRegister. If both are the NoRegister
  // then false is returned.
  bool Overlaps(const Mips64ManagedRegister& other) const;

  static Mips64ManagedRegister FromGpuRegister(GpuRegister r) {
    CHECK_NE(r, kNoGpuRegister);
    return FromRegId(r);
  }

  static Mips64ManagedRegister FromFpuRegister(FpuRegister r) {
    CHECK_NE(r, kNoFpuRegister);
    return FromRegId(r + kNumberOfGpuRegIds);
  }

 private:
  bool IsValidManagedRegister() const {
    return (0 <= id_) && (id_ < kNumberOfRegIds);
  }

  int RegId() const {
    CHECK(!IsNoRegister());
    return id_;
  }

  int AllocId() const {
    CHECK(IsValidManagedRegister());
    CHECK_LT(id_, kNumberOfAllocIds);
    return id_;
  }

  int AllocIdLow() const;
  int AllocIdHigh() const;

  friend class ManagedRegister;

  explicit Mips64ManagedRegister(int reg_id) : ManagedRegister(reg_id) {}

  static Mips64ManagedRegister FromRegId(int reg_id) {
    Mips64ManagedRegister reg(reg_id);
    CHECK(reg.IsValidManagedRegister());
    return reg;
  }
};

std::ostream& operator<<(std::ostream& os, const Mips64ManagedRegister& reg);

}  // namespace mips64

inline mips64::Mips64ManagedRegister ManagedRegister::AsMips64() const {
  mips64::Mips64ManagedRegister reg(id_);
  CHECK(reg.IsNoRegister() || reg.IsValidManagedRegister());
  return reg;
}

}  // namespace art

#endif  // ART_COMPILER_UTILS_MIPS64_MANAGED_REGISTER_MIPS64_H_
