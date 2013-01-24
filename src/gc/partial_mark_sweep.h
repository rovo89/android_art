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

#ifndef ART_SRC_GC_PARTIAL_MARK_SWEEP_H_
#define ART_SRC_GC_PARTIAL_MARK_SWEEP_H_

#include "locks.h"
#include "mark_sweep.h"

namespace art {

class PartialMarkSweep : public MarkSweep {
 public:
  virtual GcType GetGcType() const {
    return kGcTypePartial;
  }

  explicit PartialMarkSweep(Heap* heap, bool is_concurrent);
  ~PartialMarkSweep();

protected:
  virtual void BindBitmaps()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  DISALLOW_COPY_AND_ASSIGN(PartialMarkSweep);
};

}  // namespace art

#endif  // ART_SRC_GC_PARTIAL_MARK_SWEEP_H_
