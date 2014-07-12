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

#ifndef ART_RUNTIME_IMPLICIT_CHECK_OPTIONS_H_
#define ART_RUNTIME_IMPLICIT_CHECK_OPTIONS_H_

#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "instruction_set.h"
#include "runtime.h"

#include <string>

namespace art {

class ImplicitCheckOptions {
 public:
  static constexpr const char* kImplicitChecksOatHeaderKey = "implicit-checks";

  static std::string Serialize(bool explicit_null_checks, bool explicit_stack_overflow_checks,
                               bool explicit_suspend_checks) {
    char tmp[4];
    tmp[0] = explicit_null_checks ? 'N' : 'n';
    tmp[1] = explicit_stack_overflow_checks ? 'O' : 'o';
    tmp[2] = explicit_suspend_checks ? 'S' : 's';
    tmp[3] = 0;
    return std::string(tmp);
  }

  static bool Parse(const char* str, bool* explicit_null_checks,
                    bool* explicit_stack_overflow_checks, bool* explicit_suspend_checks) {
    if (str != nullptr && str[0] != 0 && str[1] != 0 && str[2] != 0 &&
        (str[0] == 'n' || str[0] == 'N') &&
        (str[1] == 'o' || str[1] == 'O') &&
        (str[2] == 's' || str[2] == 'S')) {
      *explicit_null_checks = str[0] == 'N';
      *explicit_stack_overflow_checks = str[1] == 'O';
      *explicit_suspend_checks = str[2] == 'S';
      return true;
    } else {
      return false;
    }
  }

  // Check whether the given flags are correct with respect to the current runtime and the given
  // executable flag.
  static bool CheckRuntimeSupport(bool executable, bool explicit_null_checks,
                                  bool explicit_stack_overflow_checks,
                                  bool explicit_suspend_checks, std::string* error_msg) {
    if (!executable) {
      // Not meant to be run, i.e., either we are compiling or dumping. Just accept.
      return true;
    }

    Runtime* runtime = Runtime::Current();
    // We really should have a runtime.
    DCHECK_NE(static_cast<Runtime*>(nullptr), runtime);

    if (runtime->GetInstrumentation()->IsForcedInterpretOnly()) {
      // We are an interpret-only environment. Ignore the check value.
      return true;
    }

    if (runtime->ExplicitNullChecks() != explicit_null_checks ||
        runtime->ExplicitStackOverflowChecks() != explicit_stack_overflow_checks ||
        runtime->ExplicitSuspendChecks() != explicit_suspend_checks) {
      if (error_msg != nullptr) {
        // Create an error message.

        std::ostringstream os;
        os << "Explicit check options do not match runtime: ";
        os << runtime->ExplicitNullChecks() << " vs " << explicit_null_checks << " | ";
        os << runtime->ExplicitStackOverflowChecks() << " vs " << explicit_stack_overflow_checks
            << " | ";
        os << runtime->ExplicitSuspendChecks() << " vs " << explicit_suspend_checks;

        *error_msg = os.str();
      }

      // Currently we do not create correct images when pre-opting, so the emulator will fail with
      // this change. Once the change is in the tree, REMOVE.
      if (true) {
        // At least try to log it, though.
        if (error_msg != nullptr) {
          LOG(WARNING) << *error_msg;
        }
        return true;
      } else {
        return false;
      }
    }

    // Accepted.
    return true;
  }

  // Check (and override) the flags depending on current support in the ISA.
  // Right now will reset all flags to explicit except on ARM.
  static void CheckISASupport(InstructionSet isa, bool* explicit_null_checks,
                              bool* explicit_stack_overflow_checks, bool* explicit_suspend_checks) {
    switch (isa) {
      case kArm:
      case kThumb2:
        break;  // All checks implemented, leave as is.

      default:  // No checks implemented, reset all to explicit checks.
        *explicit_null_checks = true;
        *explicit_stack_overflow_checks = true;
        *explicit_suspend_checks = true;
    }
  }

  static bool CheckForCompiling(InstructionSet host, InstructionSet target,
                                bool* explicit_null_checks, bool* explicit_stack_overflow_checks,
                                bool* explicit_suspend_checks) {
    // Check the boot image settings.
    Runtime* runtime = Runtime::Current();
    if (runtime != nullptr) {
      gc::space::ImageSpace* ispace = runtime->GetHeap()->GetImageSpace();
      if (ispace != nullptr) {
        const OatFile* oat_file = ispace->GetOatFile();
        if (oat_file != nullptr) {
          const char* v = oat_file->GetOatHeader().GetStoreValueByKey(kImplicitChecksOatHeaderKey);
          if (!Parse(v, explicit_null_checks, explicit_stack_overflow_checks,
                     explicit_suspend_checks)) {
            LOG(FATAL) << "Should have been able to parse boot image implicit check values";
          }
          return true;
        }
      }
    }

    // Check the current runtime.
    bool cross_compiling = true;
    switch (host) {
      case kArm:
      case kThumb2:
        cross_compiling = target != kArm && target != kThumb2;
        break;
      default:
        cross_compiling = host != target;
        break;
    }
    if (!cross_compiling) {
      Runtime* runtime = Runtime::Current();
      *explicit_null_checks = runtime->ExplicitNullChecks();
      *explicit_stack_overflow_checks = runtime->ExplicitStackOverflowChecks();
      *explicit_suspend_checks = runtime->ExplicitSuspendChecks();
      return true;
    }

    // Give up.
    return false;
  }
};

}  // namespace art

#endif  // ART_RUNTIME_IMPLICIT_CHECK_OPTIONS_H_
