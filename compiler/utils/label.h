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

#ifndef ART_COMPILER_UTILS_LABEL_H_
#define ART_COMPILER_UTILS_LABEL_H_

#include "base/logging.h"
#include "base/macros.h"

namespace art {

class Assembler;
class AssemblerBuffer;
class AssemblerFixup;

namespace arm {
  class ArmAssembler;
  class Arm32Assembler;
  class Thumb2Assembler;
}
namespace arm64 {
  class Arm64Assembler;
}
namespace mips {
  class MipsAssembler;
}
namespace mips64 {
  class Mips64Assembler;
}
namespace x86 {
  class X86Assembler;
  class NearLabel;
}
namespace x86_64 {
  class X86_64Assembler;
  class NearLabel;
}

class ExternalLabel {
 public:
  ExternalLabel(const char* name_in, uintptr_t address_in)
      : name_(name_in), address_(address_in) {
    DCHECK(name_in != nullptr);
  }

  const char* name() const { return name_; }
  uintptr_t address() const {
    return address_;
  }

 private:
  const char* name_;
  const uintptr_t address_;
};

class Label {
 public:
  Label() : position_(0) {}

  Label(Label&& src)
      : position_(src.position_) {
    // We must unlink/unbind the src label when moving; if not, calling the destructor on
    // the src label would fail.
    src.position_ = 0;
  }

  ~Label() {
    // Assert if label is being destroyed with unresolved branches pending.
    CHECK(!IsLinked());
  }

  // Returns the position for bound and linked labels. Cannot be used
  // for unused labels.
  int Position() const {
    CHECK(!IsUnused());
    return IsBound() ? -position_ - sizeof(void*) : position_ - sizeof(void*);
  }

  int LinkPosition() const {
    CHECK(IsLinked());
    return position_ - sizeof(void*);
  }

  bool IsBound() const { return position_ < 0; }
  bool IsUnused() const { return position_ == 0; }
  bool IsLinked() const { return position_ > 0; }

 private:
  int position_;

  void Reinitialize() {
    position_ = 0;
  }

  void BindTo(int position) {
    CHECK(!IsBound());
    position_ = -position - sizeof(void*);
    CHECK(IsBound());
  }

  void LinkTo(int position) {
    CHECK(!IsBound());
    position_ = position + sizeof(void*);
    CHECK(IsLinked());
  }

  friend class arm::ArmAssembler;
  friend class arm::Arm32Assembler;
  friend class arm::Thumb2Assembler;
  friend class arm64::Arm64Assembler;
  friend class mips::MipsAssembler;
  friend class mips64::Mips64Assembler;
  friend class x86::X86Assembler;
  friend class x86::NearLabel;
  friend class x86_64::X86_64Assembler;
  friend class x86_64::NearLabel;

  DISALLOW_COPY_AND_ASSIGN(Label);
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_LABEL_H_
