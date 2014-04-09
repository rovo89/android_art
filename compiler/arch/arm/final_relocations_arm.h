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

#ifndef ART_COMPILER_ARCH_ARM_FINAL_RELOCATIONS_ARM_H_
#define ART_COMPILER_ARCH_ARM_FINAL_RELOCATIONS_ARM_H_

// ARM final relocations

#include "final_relocations.h"

namespace art {

class CompilerDriver;
struct CompilationUnit;

class FinalEntrypointRelocationSetArm : public FinalEntrypointRelocationSet {
 public:
  explicit FinalEntrypointRelocationSetArm(const CompilerDriver* driver) : FinalEntrypointRelocationSet(driver) {}
  ~FinalEntrypointRelocationSetArm() {}

  void Apply(uint8_t* code, const OatWriter* writer, uint32_t address) const;
};

}   // namespace art

#endif  // ART_COMPILER_ARCH_ARM_FINAL_RELOCATIONS_ARM_H_
