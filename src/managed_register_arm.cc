// Copyright 2011 Google Inc. All Rights Reserved.

#include "managed_register_arm.h"

#include "globals.h"
#include "calling_convention.h"

namespace art {
namespace arm {

// We need all registers for caching of locals.
// Register R9 .. R15 are reserved.
static const int kNumberOfAvailableCoreRegisters = (R8 - R0) + 1;
static const int kNumberOfAvailableSRegisters = kNumberOfSRegisters;
static const int kNumberOfAvailableDRegisters = kNumberOfDRegisters;
static const int kNumberOfAvailableOverlappingDRegisters =
    kNumberOfOverlappingDRegisters;
static const int kNumberOfAvailableRegisterPairs = kNumberOfRegisterPairs;


// Returns true if this managed-register overlaps the other managed-register.
bool ArmManagedRegister::Overlaps(const ArmManagedRegister& other) const {
  if (IsNoRegister() || other.IsNoRegister()) return false;
  if (Equals(other)) return true;
  if (IsRegisterPair()) {
    Register low = AsRegisterPairLow();
    Register high = AsRegisterPairHigh();
    return ArmManagedRegister::FromCoreRegister(low).Overlaps(other) ||
        ArmManagedRegister::FromCoreRegister(high).Overlaps(other);
  }
  if (IsOverlappingDRegister()) {
    if (other.IsDRegister()) return Equals(other);
    if (other.IsSRegister()) {
      SRegister low = AsOverlappingDRegisterLow();
      SRegister high = AsOverlappingDRegisterHigh();
      SRegister other_sreg = other.AsSRegister();
      return (low == other_sreg) || (high == other_sreg);
    }
    return false;
  }
  if (other.IsRegisterPair() || other.IsOverlappingDRegister()) {
    return other.Overlaps(*this);
  }
  return false;
}


int ArmManagedRegister::AllocIdLow() const {
  CHECK(IsOverlappingDRegister() || IsRegisterPair());
  const int r = RegId() - (kNumberOfCoreRegIds + kNumberOfSRegIds);
  int low;
  if (r < kNumberOfOverlappingDRegIds) {
    CHECK(IsOverlappingDRegister());
    low = (r * 2) + kNumberOfCoreRegIds;  // Return a SRegister.
  } else {
    CHECK(IsRegisterPair());
    low = (r - kNumberOfDRegIds) * 2;  // Return a Register.
    if (low > 6) {
      // we didn't got a pair higher than R6_R7, must be the dalvik special case
      low = 1;
    }
  }
  return low;
}


int ArmManagedRegister::AllocIdHigh() const {
  return AllocIdLow() + 1;
}


void ArmManagedRegister::Print(std::ostream& os) const {
  if (!IsValidManagedRegister()) {
    os << "No Register";
  } else if (IsCoreRegister()) {
    os << "Core: " << static_cast<int>(AsCoreRegister());
  } else if (IsRegisterPair()) {
    os << "Pair: " << static_cast<int>(AsRegisterPairLow()) << ", "
       << static_cast<int>(AsRegisterPairHigh());
  } else if (IsSRegister()) {
    os << "SRegister: " << static_cast<int>(AsSRegister());
  } else if (IsDRegister()) {
    os << "DRegister: " << static_cast<int>(AsDRegister());
  } else {
    os << "??: " << RegId();
  }
}

std::ostream& operator<<(std::ostream& os, const ArmManagedRegister& reg) {
  reg.Print(os);
  return os;
}

std::ostream& operator<<(std::ostream& os, const RegisterPair& r) {
  os << ArmManagedRegister::FromRegisterPair(r);
  return os;
}

}  // namespace arm
}  // namespace art
