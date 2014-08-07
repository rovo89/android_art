/*
 * Copyright (C) 2009 The Android Open Source Project
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

#include "indirect_reference_table-inl.h"

#include "jni_internal.h"
#include "reference_table.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "utils.h"
#include "verify_object-inl.h"

#include <cstdlib>

namespace art {

template<typename T>
class MutatorLockedDumpable {
 public:
  explicit MutatorLockedDumpable(T& value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) : value_(value) {
  }

  void Dump(std::ostream& os) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    value_.Dump(os);
  }

 private:
  T& value_;

  DISALLOW_COPY_AND_ASSIGN(MutatorLockedDumpable);
};

template<typename T>
std::ostream& operator<<(std::ostream& os, const MutatorLockedDumpable<T>& rhs)
// TODO: should be SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) however annotalysis
//       currently fails for this.
    NO_THREAD_SAFETY_ANALYSIS {
  rhs.Dump(os);
  return os;
}

void IndirectReferenceTable::AbortIfNoCheckJNI() {
  // If -Xcheck:jni is on, it'll give a more detailed error before aborting.
  if (!Runtime::Current()->GetJavaVM()->check_jni) {
    // Otherwise, we want to abort rather than hand back a bad reference.
    LOG(FATAL) << "JNI ERROR (app bug): see above.";
  }
}

IndirectReferenceTable::IndirectReferenceTable(size_t initialCount,
                                               size_t maxCount, IndirectRefKind desiredKind) {
  CHECK_GT(initialCount, 0U);
  CHECK_LE(initialCount, maxCount);
  CHECK_NE(desiredKind, kHandleScopeOrInvalid);

  std::string error_str;
  const size_t initial_bytes = initialCount * sizeof(const mirror::Object*);
  const size_t table_bytes = maxCount * sizeof(const mirror::Object*);
  table_mem_map_.reset(MemMap::MapAnonymous("indirect ref table", nullptr, table_bytes,
                                            PROT_READ | PROT_WRITE, false, &error_str));
  CHECK(table_mem_map_.get() != nullptr) << error_str;
  CHECK_EQ(table_mem_map_->Size(), table_bytes);

  table_ = reinterpret_cast<GcRoot<mirror::Object>*>(table_mem_map_->Begin());
  CHECK(table_ != nullptr);
  memset(table_, 0xd1, initial_bytes);

  const size_t slot_bytes = maxCount * sizeof(IndirectRefSlot);
  slot_mem_map_.reset(MemMap::MapAnonymous("indirect ref table slots", nullptr, slot_bytes,
                                           PROT_READ | PROT_WRITE, false, &error_str));
  CHECK(slot_mem_map_.get() != nullptr) << error_str;
  slot_data_ = reinterpret_cast<IndirectRefSlot*>(slot_mem_map_->Begin());
  CHECK(slot_data_ != nullptr);

  segment_state_.all = IRT_FIRST_SEGMENT;
  alloc_entries_ = initialCount;
  max_entries_ = maxCount;
  kind_ = desiredKind;
}

IndirectReferenceTable::~IndirectReferenceTable() {
}

IndirectRef IndirectReferenceTable::Add(uint32_t cookie, mirror::Object* obj) {
  IRTSegmentState prevState;
  prevState.all = cookie;
  size_t topIndex = segment_state_.parts.topIndex;

  CHECK(obj != NULL);
  VerifyObject(obj);
  DCHECK(table_ != NULL);
  DCHECK_LE(alloc_entries_, max_entries_);
  DCHECK_GE(segment_state_.parts.numHoles, prevState.parts.numHoles);

  if (topIndex == alloc_entries_) {
    // reached end of allocated space; did we hit buffer max?
    if (topIndex == max_entries_) {
      LOG(FATAL) << "JNI ERROR (app bug): " << kind_ << " table overflow "
                 << "(max=" << max_entries_ << ")\n"
                 << MutatorLockedDumpable<IndirectReferenceTable>(*this);
    }

    size_t newSize = alloc_entries_ * 2;
    if (newSize > max_entries_) {
      newSize = max_entries_;
    }
    DCHECK_GT(newSize, alloc_entries_);

    alloc_entries_ = newSize;
  }

  // We know there's enough room in the table.  Now we just need to find
  // the right spot.  If there's a hole, find it and fill it; otherwise,
  // add to the end of the list.
  IndirectRef result;
  int numHoles = segment_state_.parts.numHoles - prevState.parts.numHoles;
  if (numHoles > 0) {
    DCHECK_GT(topIndex, 1U);
    // Find the first hole; likely to be near the end of the list.
    GcRoot<mirror::Object>* pScan = &table_[topIndex - 1];
    DCHECK(!pScan->IsNull());
    --pScan;
    while (!pScan->IsNull()) {
      DCHECK_GE(pScan, table_ + prevState.parts.topIndex);
      --pScan;
    }
    UpdateSlotAdd(obj, pScan - table_);
    result = ToIndirectRef(pScan - table_);
    *pScan = GcRoot<mirror::Object>(obj);
    segment_state_.parts.numHoles--;
  } else {
    // Add to the end.
    UpdateSlotAdd(obj, topIndex);
    result = ToIndirectRef(topIndex);
    table_[topIndex++] = GcRoot<mirror::Object>(obj);
    segment_state_.parts.topIndex = topIndex;
  }
  if (false) {
    LOG(INFO) << "+++ added at " << ExtractIndex(result) << " top=" << segment_state_.parts.topIndex
              << " holes=" << segment_state_.parts.numHoles;
  }

  DCHECK(result != NULL);
  return result;
}

void IndirectReferenceTable::AssertEmpty() {
  if (UNLIKELY(begin() != end())) {
    ScopedObjectAccess soa(Thread::Current());
    LOG(FATAL) << "Internal Error: non-empty local reference table\n"
               << MutatorLockedDumpable<IndirectReferenceTable>(*this);
  }
}

// Removes an object. We extract the table offset bits from "iref"
// and zap the corresponding entry, leaving a hole if it's not at the top.
// If the entry is not between the current top index and the bottom index
// specified by the cookie, we don't remove anything. This is the behavior
// required by JNI's DeleteLocalRef function.
// This method is not called when a local frame is popped; this is only used
// for explicit single removals.
// Returns "false" if nothing was removed.
bool IndirectReferenceTable::Remove(uint32_t cookie, IndirectRef iref) {
  IRTSegmentState prevState;
  prevState.all = cookie;
  int topIndex = segment_state_.parts.topIndex;
  int bottomIndex = prevState.parts.topIndex;

  DCHECK(table_ != NULL);
  DCHECK_LE(alloc_entries_, max_entries_);
  DCHECK_GE(segment_state_.parts.numHoles, prevState.parts.numHoles);

  int idx = ExtractIndex(iref);

  if (GetIndirectRefKind(iref) == kHandleScopeOrInvalid &&
      Thread::Current()->HandleScopeContains(reinterpret_cast<jobject>(iref))) {
    LOG(WARNING) << "Attempt to remove local handle scope entry from IRT, ignoring";
    return true;
  }

  if (idx < bottomIndex) {
    // Wrong segment.
    LOG(WARNING) << "Attempt to remove index outside index area (" << idx
                 << " vs " << bottomIndex << "-" << topIndex << ")";
    return false;
  }
  if (idx >= topIndex) {
    // Bad --- stale reference?
    LOG(WARNING) << "Attempt to remove invalid index " << idx
                 << " (bottom=" << bottomIndex << " top=" << topIndex << ")";
    return false;
  }

  if (idx == topIndex-1) {
    // Top-most entry.  Scan up and consume holes.

    if (!CheckEntry("remove", iref, idx)) {
      return false;
    }

    table_[idx] = GcRoot<mirror::Object>(nullptr);
    int numHoles = segment_state_.parts.numHoles - prevState.parts.numHoles;
    if (numHoles != 0) {
      while (--topIndex > bottomIndex && numHoles != 0) {
        if (false) {
          LOG(INFO) << "+++ checking for hole at " << topIndex-1
                    << " (cookie=" << cookie << ") val="
                    << table_[topIndex - 1].Read<kWithoutReadBarrier>();
        }
        if (!table_[topIndex-1].IsNull()) {
          break;
        }
        if (false) {
          LOG(INFO) << "+++ ate hole at " << (topIndex - 1);
        }
        numHoles--;
      }
      segment_state_.parts.numHoles = numHoles + prevState.parts.numHoles;
      segment_state_.parts.topIndex = topIndex;
    } else {
      segment_state_.parts.topIndex = topIndex-1;
      if (false) {
        LOG(INFO) << "+++ ate last entry " << topIndex - 1;
      }
    }
  } else {
    // Not the top-most entry.  This creates a hole.  We NULL out the
    // entry to prevent somebody from deleting it twice and screwing up
    // the hole count.
    if (table_[idx].IsNull()) {
      LOG(INFO) << "--- WEIRD: removing null entry " << idx;
      return false;
    }
    if (!CheckEntry("remove", iref, idx)) {
      return false;
    }

    table_[idx] = GcRoot<mirror::Object>(nullptr);
    segment_state_.parts.numHoles++;
    if (false) {
      LOG(INFO) << "+++ left hole at " << idx << ", holes=" << segment_state_.parts.numHoles;
    }
  }

  return true;
}

void IndirectReferenceTable::VisitRoots(RootCallback* callback, void* arg, uint32_t tid,
                                        RootType root_type) {
  for (auto ref : *this) {
    callback(ref, arg, tid, root_type);
    DCHECK(*ref != nullptr);
  }
}

void IndirectReferenceTable::Dump(std::ostream& os) const {
  os << kind_ << " table dump:\n";
  ReferenceTable::Table entries;
  for (size_t i = 0; i < Capacity(); ++i) {
    mirror::Object* obj = table_[i].Read<kWithoutReadBarrier>();
    if (UNLIKELY(obj == nullptr)) {
      // Remove NULLs.
    } else if (UNLIKELY(obj == kClearedJniWeakGlobal)) {
      // ReferenceTable::Dump() will handle kClearedJniWeakGlobal
      // while the read barrier won't.
      entries.push_back(GcRoot<mirror::Object>(obj));
    } else {
      obj = table_[i].Read();
      entries.push_back(GcRoot<mirror::Object>(obj));
    }
  }
  ReferenceTable::Dump(os, entries);
}

}  // namespace art
