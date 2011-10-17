// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_MANAGED_REGISTER_H_
#define ART_SRC_MANAGED_REGISTER_H_

namespace art {

namespace x86 {
class X86ManagedRegister;
}
namespace arm {
class ArmManagedRegister;
}

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

  x86::X86ManagedRegister AsX86() const;
  arm::ArmManagedRegister AsArm() const;

  // It is valid to invoke Equals on and with a NoRegister.
  bool Equals(const ManagedRegister& other) const {
    return id_ == other.id_;
  }

  bool IsNoRegister() const {
    return id_ == kNoRegister;
  }

  static ManagedRegister NoRegister() {
    return ManagedRegister();
  }

 protected:
  static const int kNoRegister = -1;

  ManagedRegister() : id_(kNoRegister) { }
  explicit ManagedRegister(int reg_id) : id_(reg_id) { }

  int id_;
};

}  // namespace art

#endif  // ART_SRC_MANAGED_REGISTER_H_
