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

#ifndef ART_COMPILER_DEX_PASS_H_
#define ART_COMPILER_DEX_PASS_H_

#include <string>

#include "base/macros.h"
namespace art {

// Forward declarations.
struct BasicBlock;
struct CompilationUnit;
class Pass;

// Empty Pass Data Class, can be extended by any pass extending the base Pass class.
class PassDataHolder {
};

/**
 * @class Pass
 * @brief Base Pass class, can be extended to perform a more defined way of doing the work call.
 */
class Pass {
 public:
  explicit Pass(const char* name)
    : pass_name_(name) {
  }

  virtual ~Pass() {
  }

  virtual const char* GetName() const {
    return pass_name_;
  }

  /**
   * @brief Gate for the pass: determines whether to execute the pass or not considering a CompilationUnit
   * @param data the PassDataHolder.
   * @return whether or not to execute the pass.
   */
  virtual bool Gate(const PassDataHolder* data) const {
    // Unused parameter.
    UNUSED(data);

    // Base class says yes.
    return true;
  }

  /**
   * @brief Start of the pass: called before the Worker function.
   */
  virtual void Start(PassDataHolder* data) const {
    // Unused parameter.
    UNUSED(data);
  }

  /**
   * @brief End of the pass: called after the WalkBasicBlocks function.
   */
  virtual void End(PassDataHolder* data) const {
    // Unused parameter.
    UNUSED(data);
  }

  /**
   * @param data the object containing data necessary for the pass.
   * @return whether or not there is a change when walking the BasicBlock
   */
  virtual bool Worker(const PassDataHolder* data) const {
    // Unused parameter.
    UNUSED(data);

    // BasicBlock did not change.
    return false;
  }

  static void BasePrintMessage(CompilationUnit* c_unit, const char* pass_name, const char* message, ...) {
    // Check if we want to log something or not.
    if (c_unit->print_pass) {
      // Stringify the message.
      va_list args;
      va_start(args, message);
      std::string stringified_message;
      StringAppendV(&stringified_message, message, args);
      va_end(args);

      // Log the message and ensure to include pass name.
      LOG(INFO) << pass_name << ": " << stringified_message;
    }
  }

 protected:
  /** @brief The pass name: used for searching for a pass when running a particular pass or debugging. */
  const char* const pass_name_;

 private:
  // In order to make the all passes not copy-friendly.
  DISALLOW_COPY_AND_ASSIGN(Pass);
};
}  // namespace art
#endif  // ART_COMPILER_DEX_PASS_H_
