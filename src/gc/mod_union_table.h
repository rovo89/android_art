/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_MOD_UNION_TABLE_H_
#define ART_SRC_MOD_UNION_TABLE_H_

#include "heap.h"
#include "safe_map.h"
#include "space.h"

namespace art {

class Heap;
class HeapBitmap;
class Space;

// Base class
class ModUnionTable {
 public:
  typedef std::vector<const Object*> ReferenceArray;
  typedef std::set<byte*> ClearedCards;

  ModUnionTable(Heap* heap) : heap_(heap) {

  }

  virtual ~ModUnionTable() {

  }

  // Clear cards which map to a memory range of a space.
  virtual void ClearCards(ContinuousSpace* space) = 0;

  // Update the mod-union table.
  virtual void Update() = 0;

  // Mark all references which are stored in the mod union table.
  virtual void MarkReferences(MarkSweep* mark_sweep) = 0;

  // Verification, sanity checks that we don't have clean cards which conflict with out cached data
  // for said cards. Exclusive lock is required since verify sometimes uses
  // SpaceBitmap::VisitMarkedRange and VisitMarkedRange can't know if the callback will modify the
  // bitmap or not.
  virtual void Verify() EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) = 0;

  Heap* GetHeap() {
    return heap_;
  }

 protected:
  Heap* heap_;
};

// Bitmap implementation.
// DEPRECATED, performs strictly less well than merely caching which cards were dirty.
class ModUnionTableBitmap : public ModUnionTable {
 public:
  ModUnionTableBitmap(Heap* heap);
  virtual ~ModUnionTableBitmap();

  // Clear space cards.
  void ClearCards(ContinuousSpace* space);

  // Update table based on cleared cards.
  void Update()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Mark all references to the alloc space(s).
  void MarkReferences(MarkSweep* mark_sweep) EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

 protected:
  // Cleared card array, used to update the mod-union table.
  std::vector<byte*> cleared_cards_;

  // One bitmap per image space.
  // TODO: Add support for Zygote spaces?
  typedef SafeMap<ContinuousSpace*, SpaceBitmap*> BitmapMap;
  BitmapMap bitmaps_;
};

// Reference caching implementation. Caches references pointing to alloc space(s) for each card.
class ModUnionTableReferenceCache : public ModUnionTable {
 public:
  typedef SafeMap<const byte*, ReferenceArray > ReferenceMap;

  ModUnionTableReferenceCache(Heap* heap);
  virtual ~ModUnionTableReferenceCache();

  // Clear and store cards for a space.
  void ClearCards(ContinuousSpace* space);

  // Update table based on cleared cards.
  void Update()
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Mark all references to the alloc space(s).
  void MarkReferences(MarkSweep* mark_sweep) EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Exclusive lock is required since verify uses SpaceBitmap::VisitMarkedRange and
  // VisitMarkedRange can't know if the callback will modify the bitmap or not.
  void Verify() EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_);

  // Function that tells whether or not to add a reference to the table.
  virtual bool AddReference(const Object* obj, const Object* ref) = 0;

 protected:
  // Cleared card array, used to update the mod-union table.
  ClearedCards cleared_cards_;

  // Maps from dirty cards to their corresponding alloc space references.
  ReferenceMap references_;
};

// Card caching implementation. Keeps track of which cards we cleared and only this information.
class ModUnionTableCardCache : public ModUnionTable {
 public:
  typedef SafeMap<const byte*, ReferenceArray > ReferenceMap;

  ModUnionTableCardCache(Heap* heap);
  virtual ~ModUnionTableCardCache();

  // Clear and store cards for a space.
  void ClearCards(ContinuousSpace* space);

  // Nothing to update.
  void Update() {}

  // Mark all references to the alloc space(s).
  void MarkReferences(MarkSweep* mark_sweep)
      EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Nothing to verify.
  void Verify() {}

 protected:
  // Cleared card array, used to update the mod-union table.
  ClearedCards cleared_cards_;
};

template <typename Implementation>
class ModUnionTableToZygoteAllocspace : public Implementation {
public:
  ModUnionTableToZygoteAllocspace(Heap* heap) : Implementation(heap) {
  }

  bool AddReference(const Object* /* obj */, const Object* ref) {
    const Spaces& spaces = Implementation::GetHeap()->GetSpaces();
    for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
      if ((*it)->Contains(ref)) {
        return (*it)->IsAllocSpace();
      }
    }
    // Assume it points to a large object.
    // TODO: Check.
    return true;
  }
};

template <typename Implementation>
class ModUnionTableToAllocspace : public Implementation {
public:
  ModUnionTableToAllocspace(Heap* heap) : Implementation(heap) {
  }

  bool AddReference(const Object* /* obj */, const Object* ref) {
    const Spaces& spaces = Implementation::GetHeap()->GetSpaces();
    for (Spaces::const_iterator it = spaces.begin(); it != spaces.end(); ++it) {
      if ((*it)->Contains(ref)) {
        return (*it)->GetGcRetentionPolicy() == kGcRetentionPolicyAlwaysCollect;
      }
    }
    // Assume it points to a large object.
    // TODO: Check.
    return true;
  }
};

}  // namespace art

#endif  // ART_SRC_MOD_UNION_TABLE_H_
