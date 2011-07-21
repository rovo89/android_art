// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_MANAGED_REGISTER_X86_H_
#define ART_SRC_MANAGED_REGISTER_X86_H_

#include "constants_x86.h"

namespace art {

// Values for register pairs.
// The registers in kReservedCpuRegistersArray in x86.cc are not used in pairs.
// The table kRegisterPairs in x86.cc must be kept in sync with this enum.
enum RegisterPair {
  EAX_EDX = 0,
  EAX_ECX = 1,
  EAX_EBX = 2,
  EAX_EDI = 3,
  EDX_ECX = 4,
  EDX_EBX = 5,
  EDX_EDI = 6,
  ECX_EBX = 7,
  ECX_EDI = 8,
  EBX_EDI = 9,
  kNumberOfRegisterPairs = 10,
  kNoRegisterPair = -1,
};

std::ostream& operator<<(std::ostream& os, const RegisterPair& reg);

const int kNumberOfCpuRegIds = kNumberOfCpuRegisters;
const int kNumberOfCpuAllocIds = kNumberOfCpuRegisters;

const int kNumberOfXmmRegIds = kNumberOfXmmRegisters;
const int kNumberOfXmmAllocIds = kNumberOfXmmRegisters;

const int kNumberOfX87RegIds = kNumberOfX87Registers;
const int kNumberOfX87AllocIds = kNumberOfX87Registers;

const int kNumberOfPairRegIds = kNumberOfRegisterPairs;

const int kNumberOfRegIds = kNumberOfCpuRegIds + kNumberOfXmmRegIds +
    kNumberOfX87RegIds + kNumberOfPairRegIds;
const int kNumberOfAllocIds = kNumberOfCpuAllocIds + kNumberOfXmmAllocIds +
    kNumberOfX87RegIds;

// Register ids map:
//   [0..R[  cpu registers (enum Register)
//   [R..X[  xmm registers (enum XmmRegister)
//   [X..S[  x87 registers (enum X87Register)
//   [S..P[  register pairs (enum RegisterPair)
// where
//   R = kNumberOfCpuRegIds
//   X = R + kNumberOfXmmRegIds
//   S = X + kNumberOfX87RegIds
//   P = X + kNumberOfRegisterPairs

// Allocation ids map:
//   [0..R[  cpu registers (enum Register)
//   [R..X[  xmm registers (enum XmmRegister)
//   [X..S[  x87 registers (enum X87Register)
// where
//   R = kNumberOfCpuRegIds
//   X = R + kNumberOfXmmRegIds
//   S = X + kNumberOfX87RegIds


// An instance of class 'ManagedRegister' represents a single cpu register (enum
// Register), an xmm register (enum XmmRegister), or a pair of cpu registers
// (enum RegisterPair).
// 'ManagedRegister::NoRegister()' provides an invalid register.
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

  Register AsCpuRegister() const {
    CHECK(IsCpuRegister());
    return static_cast<Register>(id_);
  }

  XmmRegister AsXmmRegister() const {
    CHECK(IsXmmRegister());
    return static_cast<XmmRegister>(id_ - kNumberOfCpuRegIds);
  }

  X87Register AsX87Register() const {
    CHECK(IsX87Register());
    return static_cast<X87Register>(id_ -
                                    (kNumberOfCpuRegIds + kNumberOfXmmRegIds));
  }

  Register AsRegisterPairLow() const {
    CHECK(IsRegisterPair());
    // Appropriate mapping of register ids allows to use AllocIdLow().
    return FromRegId(AllocIdLow()).AsCpuRegister();
  }

  Register AsRegisterPairHigh() const {
    CHECK(IsRegisterPair());
    // Appropriate mapping of register ids allows to use AllocIdHigh().
    return FromRegId(AllocIdHigh()).AsCpuRegister();
  }

  bool IsCpuRegister() const {
    CHECK(IsValidManagedRegister());
    return (0 <= id_) && (id_ < kNumberOfCpuRegIds);
  }

  bool IsXmmRegister() const {
    CHECK(IsValidManagedRegister());
    const int test = id_ - kNumberOfCpuRegIds;
    return (0 <= test) && (test < kNumberOfXmmRegIds);
  }

  bool IsX87Register() const {
    CHECK(IsValidManagedRegister());
    const int test = id_ - (kNumberOfCpuRegIds + kNumberOfXmmRegIds);
    return (0 <= test) && (test < kNumberOfXmmRegIds);
  }

  bool IsRegisterPair() const {
    CHECK(IsValidManagedRegister());
    const int test = id_ -
        (kNumberOfCpuRegIds + kNumberOfXmmRegIds + kNumberOfX87RegIds);
    return (0 <= test) && (test < kNumberOfPairRegIds);
  }

  bool IsNoRegister() const {
    return id_ == kNoRegister;
  }

  void Print(std::ostream& os) const;

  // It is valid to invoke Equals on and with a NoRegister.
  bool Equals(const ManagedRegister& other) const {
    return id_ == other.id_;
  }

  // Returns true if the two managed-registers ('this' and 'other') overlap.
  // Either managed-register may be the NoRegister. If both are the NoRegister
  // then false is returned.
  bool Overlaps(const ManagedRegister& other) const;

  static ManagedRegister NoRegister() {
    return ManagedRegister();
  }

  static ManagedRegister FromCpuRegister(Register r) {
    CHECK_NE(r, kNoRegister);
    return FromRegId(r);
  }

  static ManagedRegister FromXmmRegister(XmmRegister r) {
    CHECK_NE(r, kNoXmmRegister);
    return FromRegId(r + kNumberOfCpuRegIds);
  }

  static ManagedRegister FromX87Register(X87Register r) {
    CHECK_NE(r, kNoX87Register);
    return FromRegId(r + kNumberOfCpuRegIds + kNumberOfXmmRegIds);
  }

  static ManagedRegister FromRegisterPair(RegisterPair r) {
    CHECK_NE(r, kNoRegisterPair);
    return FromRegId(r + (kNumberOfCpuRegIds + kNumberOfXmmRegIds +
                          kNumberOfX87RegIds));
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
    CHECK(IsValidManagedRegister() && !IsRegisterPair());
    CHECK_LT(id_, kNumberOfAllocIds);
    return id_;
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

#endif  // ART_SRC_MANAGED_REGISTER_X86_H_
