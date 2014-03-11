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

#ifndef ART_COMPILER_DEX_VERIFIED_METHOD_H_
#define ART_COMPILER_DEX_VERIFIED_METHOD_H_

#include <vector>

#include "base/mutex.h"
#include "method_reference.h"
#include "safe_map.h"

namespace art {

namespace verifier {
class MethodVerifier;
}  // namespace verifier

class VerifiedMethod {
 public:
  // Cast elision set type.
  // Since we're adding the dex PCs to the set in increasing order, a sorted vector
  // is better for performance (not just memory usage), especially for large sets.
  typedef std::vector<uint32_t> SafeCastSet;

  // Devirtualization map type maps dex offset to concrete method reference.
  typedef SafeMap<uint32_t, MethodReference> DevirtualizationMap;

  static const VerifiedMethod* Create(verifier::MethodVerifier* method_verifier, bool compile)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  ~VerifiedMethod() = default;

  const std::vector<uint8_t>& GetDexGcMap() const {
    return dex_gc_map_;
  }

  const DevirtualizationMap& GetDevirtMap() const {
    return devirt_map_;
  }

  const SafeCastSet& GetSafeCastSet() const {
    return safe_cast_set_;
  }

  // Returns the devirtualization target method, or nullptr if none.
  const MethodReference* GetDevirtTarget(uint32_t dex_pc) const;

  // Returns true if the cast can statically be verified to be redundant
  // by using the check-cast elision peephole optimization in the verifier.
  bool IsSafeCast(uint32_t pc) const;

 private:
  VerifiedMethod() = default;

  /*
   * Generate the GC map for a method that has just been verified (i.e. we're doing this as part of
   * verification). For type-precise determination we have all the data we need, so we just need to
   * encode it in some clever fashion.
   * Stores the data in dex_gc_map_, returns true on success and false on failure.
   */
  bool GenerateGcMap(verifier::MethodVerifier* method_verifier);

  // Verify that the GC map associated with method_ is well formed.
  static void VerifyGcMap(verifier::MethodVerifier* method_verifier,
                          const std::vector<uint8_t>& data);

  // Compute sizes for GC map data.
  static void ComputeGcMapSizes(verifier::MethodVerifier* method_verifier,
                                size_t* gc_points, size_t* ref_bitmap_bits, size_t* log2_max_gc_pc);

  // Generate devirtualizaion map into devirt_map_.
  void GenerateDevirtMap(verifier::MethodVerifier* method_verifier)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Generate safe case set into safe_cast_set_.
  void GenerateSafeCastSet(verifier::MethodVerifier* method_verifier)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  std::vector<uint8_t> dex_gc_map_;
  DevirtualizationMap devirt_map_;
  SafeCastSet safe_cast_set_;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_VERIFIED_METHOD_H_
