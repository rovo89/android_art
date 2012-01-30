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

#ifndef ART_SRC_CONTEXT_H_
#define ART_SRC_CONTEXT_H_

#include <stddef.h>
#include <stdint.h>

namespace art {

class Frame;

// Representation of a thread's context on the executing machine
class Context {
 public:
  // Creates a context for the running architecture
  static Context* Create();

  virtual ~Context() {}

  // Read values from callee saves in the given frame. The frame also holds
  // the method that holds the layout.
  virtual void FillCalleeSaves(const Frame& fr) = 0;

  // Set the stack pointer value
  virtual void SetSP(uintptr_t new_sp) = 0;

  // Set the program counter value
  virtual void SetPC(uintptr_t new_pc) = 0;

  // Read the given GPR
  virtual uintptr_t GetGPR(uint32_t reg) = 0;

  // Switch execution of the executing context to this context
  virtual void DoLongJump() = 0;
};

class VmapTable {
 public:
  explicit VmapTable(const uint16_t* table) : table_(table) {
  }

  uint16_t operator[](size_t i) const {
    return table_[i + 1];
  }

  size_t size() const {
    return table_[0];
  }

  // Is register 'reg' in the context or on the stack?
  bool IsInContext(size_t reg, uint32_t& vmap_offset) const {
    vmap_offset = 0xEBAD0FF5;
    // TODO: take advantage of the registers being ordered
    for (size_t i = 0; i < size(); ++i) {
      // Stop if we find what we are are looking for...
      if (table_[i + 1] == reg) {
        vmap_offset = i;
        return true;
      }
      // ...or the INVALID_VREG that marks lr.
      // TODO: x86?
      if (table_[i + 1] == 0xffff) {
        break;
      }
    }
    return false;
  }

 private:
  const uint16_t* table_;
};

}  // namespace art

#endif  // ART_SRC_CONTEXT_H_
