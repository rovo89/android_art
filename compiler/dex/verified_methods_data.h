/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_COMPILER_DEX_VERIFIED_METHODS_DATA_H_
#define ART_COMPILER_DEX_VERIFIED_METHODS_DATA_H_

#include <stdint.h>
#include <set>
#include <vector>

#include "base/macros.h"
#include "base/mutex.h"
#include "class_reference.h"
#include "method_reference.h"
#include "safe_map.h"

namespace art {

namespace verifier {
class MethodVerifier;
}  // namespace verifier

class VerifiedMethodsData {
  public:
    VerifiedMethodsData();
    ~VerifiedMethodsData();

    bool ProcessVerifiedMethod(verifier::MethodVerifier* method_verifier)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
        LOCKS_EXCLUDED(dex_gc_maps_lock_, devirt_maps_lock_, safecast_map_lock_);

    const std::vector<uint8_t>* GetDexGcMap(MethodReference ref)
        LOCKS_EXCLUDED(dex_gc_maps_lock_);

    const MethodReference* GetDevirtMap(const MethodReference& ref, uint32_t dex_pc)
        LOCKS_EXCLUDED(devirt_maps_lock_);

    // Returns true if the cast can statically be verified to be redundant
    // by using the check-cast elision peephole optimization in the verifier
    bool IsSafeCast(MethodReference ref, uint32_t pc) LOCKS_EXCLUDED(safecast_map_lock_);

    void AddRejectedClass(ClassReference ref) LOCKS_EXCLUDED(rejected_classes_lock_);
    bool IsClassRejected(ClassReference ref) LOCKS_EXCLUDED(rejected_classes_lock_);

    static bool IsCandidateForCompilation(MethodReference& method_ref,
                                          const uint32_t access_flags);

  private:
    /*
     * Generate the GC map for a method that has just been verified (i.e. we're doing this as part of
     * verification). For type-precise determination we have all the data we need, so we just need to
     * encode it in some clever fashion.
     * Returns a pointer to a newly-allocated RegisterMap, or NULL on failure.
     */
    const std::vector<uint8_t>* GenerateGcMap(verifier::MethodVerifier* method_verifier);

    // Verify that the GC map associated with method_ is well formed
    void VerifyGcMap(verifier::MethodVerifier* method_verifier, const std::vector<uint8_t>& data);

    // Compute sizes for GC map data
    void ComputeGcMapSizes(verifier::MethodVerifier* method_verifier,
                           size_t* gc_points, size_t* ref_bitmap_bits, size_t* log2_max_gc_pc);

    // All the GC maps that the verifier has created
    typedef SafeMap<const MethodReference, const std::vector<uint8_t>*,
        MethodReferenceComparator> DexGcMapTable;
    ReaderWriterMutex dex_gc_maps_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
    DexGcMapTable dex_gc_maps_ GUARDED_BY(dex_gc_maps_lock_);
    void SetDexGcMap(MethodReference ref, const std::vector<uint8_t>* dex_gc_map)
        LOCKS_EXCLUDED(dex_gc_maps_lock_);

    // Cast elision types.
    typedef std::set<uint32_t> MethodSafeCastSet;
    typedef SafeMap<MethodReference, const MethodSafeCastSet*,
        MethodReferenceComparator> SafeCastMap;
    MethodSafeCastSet* GenerateSafeCastSet(verifier::MethodVerifier* method_verifier)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
    void SetSafeCastMap(MethodReference ref, const MethodSafeCastSet* mscs)
        LOCKS_EXCLUDED(safecast_map_lock_);
    ReaderWriterMutex safecast_map_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
    SafeCastMap safecast_map_ GUARDED_BY(safecast_map_lock_);

    // Devirtualization map.
    typedef SafeMap<uint32_t, MethodReference> PcToConcreteMethodMap;
    typedef SafeMap<MethodReference, const PcToConcreteMethodMap*,
        MethodReferenceComparator> DevirtualizationMapTable;
    PcToConcreteMethodMap* GenerateDevirtMap(verifier::MethodVerifier* method_verifier)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
    ReaderWriterMutex devirt_maps_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
    DevirtualizationMapTable devirt_maps_ GUARDED_BY(devirt_maps_lock_);
    void SetDevirtMap(MethodReference ref, const PcToConcreteMethodMap* pc_method_map)
          LOCKS_EXCLUDED(devirt_maps_lock_);

    // Rejected classes
    typedef std::set<ClassReference> RejectedClassesTable;
    ReaderWriterMutex rejected_classes_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
    RejectedClassesTable rejected_classes_ GUARDED_BY(rejected_classes_lock_);
};

}  // namespace art

#endif  // ART_COMPILER_DEX_VERIFIED_METHODS_DATA_H_
