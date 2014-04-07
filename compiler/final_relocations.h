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

#ifndef ART_COMPILER_FINAL_RELOCATIONS_H_
#define ART_COMPILER_FINAL_RELOCATIONS_H_

#include <string>
#include <vector>
#include "base/macros.h"

namespace art {

class CompilerDriver;
class OatWriter;

enum FinalRelocationType {
  kRelocationCall       // Relocation of a call instruction.
};

// This is a set of relocations that is performed when the code is finally
// written to the output file.  This is when we know all the offsets and
// can patch the binary instructions with known PC relative addresses.
//
// This is an abstract class that can be used for sets of relocations of different
// types.  For example, one type of relocation set is the relocation of calls
// to entrypoint trampoline islands.  Another type could be intra-app direct
// method calls.  The 'Apply' function is virtual and is implemented by
// each concrete subclass.
class FinalRelocationSet {
 public:
  explicit FinalRelocationSet(const CompilerDriver* driver) : driver_(driver) {}
  virtual ~FinalRelocationSet() {}

  void AddRelocation(FinalRelocationType type, uint32_t offset, uintptr_t value) {
    relocations_.push_back(Relocation(type, offset, value));
  }

  // Apply this relocation set to the given code.
  virtual void Apply(uint8_t* code, const OatWriter* writer, uint32_t address) const = 0;

 protected:
  struct Relocation {
    Relocation()=delete;
    Relocation(FinalRelocationType type, uint32_t offset, uintptr_t value) :
      type_(type), code_offset_(offset), value_(value) {}
    FinalRelocationType type_;
    uint32_t code_offset_;
    uintptr_t value_;
  };

  const CompilerDriver* const driver_;
  typedef std::vector<Relocation> Relocations;
  Relocations relocations_;

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(FinalRelocationSet);
};

/* abstract.  Implemented by architecture-specific subclasses */
class FinalEntrypointRelocationSet : public FinalRelocationSet {
 public:
  explicit FinalEntrypointRelocationSet(const CompilerDriver* driver) : FinalRelocationSet(driver) {
  }
  ~FinalEntrypointRelocationSet() {}

  void Add(uint32_t offset, uint32_t entrypoint_offset);
};

// Holder class for a set of final relocations.
class FinalRelocations : public std::vector<const FinalRelocationSet*> {
 public:
  void Apply(uint8_t* code, const OatWriter* writer, uint32_t address) const;
};
}  // namespace art

#endif  // ART_COMPILER_FINAL_RELOCATIONS_H_
