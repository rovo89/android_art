/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_COMPILER_UTILS_MANAGED_REGISTER_H_
#define ART_COMPILER_UTILS_MANAGED_REGISTER_H_

#include <vector>

namespace art {

namespace arm {
class ArmManagedRegister;
}
namespace arm64 {
class Arm64ManagedRegister;
}
namespace mips {
class MipsManagedRegister;
}

namespace x86 {
class X86ManagedRegister;
}

namespace x86_64 {
class X86_64ManagedRegister;
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

  arm::ArmManagedRegister AsArm() const;
  arm64::Arm64ManagedRegister AsArm64() const;
  mips::MipsManagedRegister AsMips() const;
  x86::X86ManagedRegister AsX86() const;
  x86_64::X86_64ManagedRegister AsX86_64() const;

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

  int RegId() const { return id_; }
  explicit ManagedRegister(int reg_id) : id_(reg_id) { }

 protected:
  static const int kNoRegister = -1;

  ManagedRegister() : id_(kNoRegister) { }

  int id_;
};

class ManagedRegisterSpill : public ManagedRegister {
 public:
  // ManagedRegisterSpill contains information about data type size and location in caller frame
  // These additional attributes could be defined by calling convention (EntrySpills)
  ManagedRegisterSpill(const ManagedRegister& other, uint32_t size, uint32_t spill_offset)
      : ManagedRegister(other), size_(size), spill_offset_(spill_offset)  { }

  explicit ManagedRegisterSpill(const ManagedRegister& other)
      : ManagedRegister(other), size_(-1), spill_offset_(-1) { }

  explicit ManagedRegisterSpill(const ManagedRegister& other, int32_t size)
      : ManagedRegister(other), size_(size), spill_offset_(-1) { }

  int32_t getSpillOffset() {
    return spill_offset_;
  }

  int32_t getSize() {
    return size_;
  }

 private:
  int32_t size_;
  int32_t spill_offset_;
};

class ManagedRegisterEntrySpills : public std::vector<ManagedRegisterSpill> {
 public:
  // The ManagedRegister does not have information about size and offset.
  // In this case it's size and offset determined by BuildFrame (assembler)
  void push_back(ManagedRegister __x) {
    ManagedRegisterSpill spill(__x);
    std::vector<ManagedRegisterSpill>::push_back(spill);
  }

  void push_back(ManagedRegister __x, int32_t __size) {
    ManagedRegisterSpill spill(__x, __size);
    std::vector<ManagedRegisterSpill>::push_back(spill);
  }

  void push_back(ManagedRegisterSpill __x) {
    std::vector<ManagedRegisterSpill>::push_back(__x);
  }
 private:
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_MANAGED_REGISTER_H_
