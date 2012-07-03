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

#include "safe_map.h"

namespace art {

class Heap;
class HeapBitmap;
class Space;

class ModUnionTable {
 public:
  // Clear cards image space cards.
  virtual void ClearCards() = 0;

  // Update the mod-union table.
  virtual void Update(MarkSweep* mark_sweep) = 0;

  // Mark all references to the alloc space(s).
  virtual void MarkReferences(MarkSweep* mark_sweep) = 0;

  virtual ~ModUnionTable() {

  }
};

// Bitmap implementation.
class ModUnionTableBitmap : public ModUnionTable {
 public:
  ModUnionTableBitmap(Heap* heap);
  virtual ~ModUnionTableBitmap();

  void Init();

  // Clear image space cards.
  void ClearCards();

  // Update table based on cleared cards.
  void Update(MarkSweep* mark_sweep);

  // Mark all references to the alloc space(s).
  void MarkReferences(MarkSweep* mark_sweep);
 private:
  // Cleared card array, used to update the mod-union table.
  std::vector<byte*> cleared_cards_;

  // One bitmap per image space.
  // TODO: Add support for zygote spaces?
  typedef SafeMap<Space*,  SpaceBitmap*> BitmapMap;
  BitmapMap bitmaps_;

  Heap* heap_;
};

}  // namespace art

#endif  // ART_SRC_MOD_UNION_TABLE_H_
