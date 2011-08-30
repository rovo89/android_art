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

#include "indirect_reference_table.h"
#include "jni_internal.h"
#include "reference_table.h"
#include "runtime.h"
#include "utils.h"

#include <cstdlib>

namespace art {

static void AbortMaybe() {
  // If -Xcheck:jni is on, it'll give a more detailed error before aborting.
  if (!Runtime::Current()->GetJavaVM()->check_jni) {
    // Otherwise, we want to abort rather than hand back a bad reference.
    LOG(FATAL) << "JNI ERROR (app bug): see above.";
  }
}

IndirectReferenceTable::IndirectReferenceTable(size_t initialCount,
    size_t maxCount, IndirectRefKind desiredKind)
{
  CHECK_GT(initialCount, 0U);
  CHECK_LE(initialCount, maxCount);
  CHECK_NE(desiredKind, kSirtOrInvalid);

  table_ = reinterpret_cast<Object**>(malloc(initialCount * sizeof(Object*)));
  CHECK(table_ != NULL);
#ifndef NDEBUG
  memset(table_, 0xd1, initialCount * sizeof(Object*));
#endif

  slot_data_ = reinterpret_cast<IndirectRefSlot*>(calloc(initialCount, sizeof(IndirectRefSlot)));
  CHECK(slot_data_ != NULL);

  segmentState.all = IRT_FIRST_SEGMENT;
  alloc_entries_ = initialCount;
  max_entries_ = maxCount;
  kind_ = desiredKind;
}

IndirectReferenceTable::~IndirectReferenceTable() {
  free(table_);
  free(slot_data_);
  table_ = NULL;
  slot_data_ = NULL;
  alloc_entries_ = max_entries_ = -1;
}

/*
 * Make sure that the entry at "idx" is correctly paired with "iref".
 */
bool IndirectReferenceTable::CheckEntry(const char* what, IndirectRef iref, int idx) const {
  Object* obj = table_[idx];
  IndirectRef checkRef = ToIndirectRef(obj, idx);
  if (checkRef != iref) {
    LOG(ERROR) << "JNI ERROR (app bug): attempt to " << what
               << " stale " << kind_ << " " << iref
               << " (should be " << checkRef << ")";
    AbortMaybe();
    return false;
  }
  return true;
}

IndirectRef IndirectReferenceTable::Add(uint32_t cookie, Object* obj) {
  IRTSegmentState prevState;
  prevState.all = cookie;
  size_t topIndex = segmentState.parts.topIndex;

  DCHECK(obj != NULL);
  // TODO: stronger sanity check on the object (such as in heap)
  DCHECK(IsAligned(reinterpret_cast<intptr_t>(obj), 8));
  DCHECK(table_ != NULL);
  DCHECK_LE(alloc_entries_, max_entries_);
  DCHECK_GE(segmentState.parts.numHoles, prevState.parts.numHoles);

  if (topIndex == alloc_entries_) {
    /* reached end of allocated space; did we hit buffer max? */
    if (topIndex == max_entries_) {
      LOG(ERROR) << "JNI ERROR (app bug): " << kind_ << " table overflow "
                 << "(max=" << max_entries_ << ")";
      Dump();
      LOG(FATAL); // TODO: operator<< for IndirectReferenceTable
    }

    size_t newSize = alloc_entries_ * 2;
    if (newSize > max_entries_) {
      newSize = max_entries_;
    }
    DCHECK_GT(newSize, alloc_entries_);

    table_ = (Object**) realloc(table_, newSize * sizeof(Object*));
    slot_data_ = (IndirectRefSlot*) realloc(slot_data_, newSize * sizeof(IndirectRefSlot));
    if (table_ == NULL || slot_data_ == NULL) {
      LOG(ERROR) << "JNI ERROR (app bug): unable to expand "
                 << kind_ << " table (from "
                 << alloc_entries_ << " to " << newSize
                 << ", max=" << max_entries_ << ")";
      Dump();
      LOG(FATAL); // TODO: operator<< for IndirectReferenceTable
    }

    // Clear the newly-allocated slot_data_ elements.
    memset(slot_data_ + alloc_entries_, 0, (newSize - alloc_entries_) * sizeof(IndirectRefSlot));

    alloc_entries_ = newSize;
  }

  /*
   * We know there's enough room in the table.  Now we just need to find
   * the right spot.  If there's a hole, find it and fill it; otherwise,
   * add to the end of the list.
   */
  IndirectRef result;
  int numHoles = segmentState.parts.numHoles - prevState.parts.numHoles;
  if (numHoles > 0) {
    DCHECK_GT(topIndex, 1U);
    /* find the first hole; likely to be near the end of the list */
    Object** pScan = &table_[topIndex - 1];
    DCHECK(*pScan != NULL);
    while (*--pScan != NULL) {
      DCHECK_GE(pScan, table_ + prevState.parts.topIndex);
    }
    UpdateSlotAdd(obj, pScan - table_);
    result = ToIndirectRef(obj, pScan - table_);
    *pScan = obj;
    segmentState.parts.numHoles--;
  } else {
    /* add to the end */
    UpdateSlotAdd(obj, topIndex);
    result = ToIndirectRef(obj, topIndex);
    table_[topIndex++] = obj;
    segmentState.parts.topIndex = topIndex;
  }

  DCHECK(result != NULL);
  return result;
}

/*
 * Verify that the indirect table lookup is valid.
 *
 * Returns "false" if something looks bad.
 */
bool IndirectReferenceTable::GetChecked(IndirectRef iref) const {
  if (iref == NULL) {
    LOG(WARNING) << "Attempt to look up NULL " << kind_;
    return false;
  }
  if (GetIndirectRefKind(iref) == kSirtOrInvalid) {
    LOG(ERROR) << "JNI ERROR (app bug): invalid " << kind_ << " " << iref;
    AbortMaybe();
    return false;
  }

  int topIndex = segmentState.parts.topIndex;
  int idx = ExtractIndex(iref);
  if (idx >= topIndex) {
    /* bad -- stale reference? */
    LOG(ERROR) << "JNI ERROR (app bug): accessed stale " << kind_ << " " << iref << " (index " << idx << " in a table of size " << topIndex << ")";
    AbortMaybe();
    return false;
  }

  if (table_[idx] == NULL) {
    LOG(ERROR) << "JNI ERROR (app bug): accessed deleted " << kind_ << " " << iref;
    AbortMaybe();
    return false;
  }

  if (!CheckEntry("use", iref, idx)) {
    return false;
  }

  return true;
}

static int LinearScan(IndirectRef iref, int bottomIndex, int topIndex, Object** table) {
  for (int i = bottomIndex; i < topIndex; ++i) {
    if (table[i] == reinterpret_cast<Object*>(iref)) {
      return i;
    }
  }
  return -1;
}

bool IndirectReferenceTable::Contains(IndirectRef iref) const {
  return LinearScan(iref, 0, segmentState.parts.topIndex, table_) != -1;
}

/*
 * Remove "obj" from "pRef".  We extract the table offset bits from "iref"
 * and zap the corresponding entry, leaving a hole if it's not at the top.
 *
 * If the entry is not between the current top index and the bottom index
 * specified by the cookie, we don't remove anything.  This is the behavior
 * required by JNI's DeleteLocalRef function.
 *
 * Note this is NOT called when a local frame is popped.  This is only used
 * for explicit single removals.
 *
 * Returns "false" if nothing was removed.
 */
bool IndirectReferenceTable::Remove(uint32_t cookie, IndirectRef iref) {
  IRTSegmentState prevState;
  prevState.all = cookie;
  int topIndex = segmentState.parts.topIndex;
  int bottomIndex = prevState.parts.topIndex;

  DCHECK(table_ != NULL);
  DCHECK_LE(alloc_entries_, max_entries_);
  DCHECK_GE(segmentState.parts.numHoles, prevState.parts.numHoles);

  int idx = ExtractIndex(iref);
  bool workAroundAppJniBugs = false;

  if (GetIndirectRefKind(iref) == kSirtOrInvalid /*&& gDvmJni.workAroundAppJniBugs*/) { // TODO
    idx = LinearScan(iref, bottomIndex, topIndex, table_);
    workAroundAppJniBugs = true;
    if (idx == -1) {
      LOG(WARNING) << "trying to work around app JNI bugs, but didn't find " << iref << " in table!";
      return false;
    }
  }

  if (idx < bottomIndex) {
    /* wrong segment */
    LOG(INFO) << "Attempt to remove index outside index area (" << idx << " vs " << bottomIndex << "-" << topIndex << ")";
    return false;
  }
  if (idx >= topIndex) {
    /* bad -- stale reference? */
    LOG(INFO) << "Attempt to remove invalid index " << idx << " (bottom=" << bottomIndex << " top=" << topIndex << ")";
    return false;
  }

  if (idx == topIndex-1) {
    // Top-most entry.  Scan up and consume holes.

    if (workAroundAppJniBugs == false && !CheckEntry("remove", iref, idx)) {
      return false;
    }

    table_[idx] = NULL;
    int numHoles = segmentState.parts.numHoles - prevState.parts.numHoles;
    if (numHoles != 0) {
      while (--topIndex > bottomIndex && numHoles != 0) {
        //LOG(INFO) << "+++ checking for hole at " << topIndex-1 << " (cookie=" << cookie << ") val=" << table_[topIndex-1];
        if (table_[topIndex-1] != NULL) {
          break;
        }
        //LOG(INFO) << "+++ ate hole at " << (topIndex-1);
        numHoles--;
      }
      segmentState.parts.numHoles = numHoles + prevState.parts.numHoles;
      segmentState.parts.topIndex = topIndex;
    } else {
      segmentState.parts.topIndex = topIndex-1;
      //LOG(INFO) << "+++ ate last entry " << topIndex-1;
    }
  } else {
    /*
     * Not the top-most entry.  This creates a hole.  We NULL out the
     * entry to prevent somebody from deleting it twice and screwing up
     * the hole count.
     */
    if (table_[idx] == NULL) {
      LOG(INFO) << "--- WEIRD: removing null entry " << idx;
      return false;
    }
    if (workAroundAppJniBugs == false && !CheckEntry("remove", iref, idx)) {
      return false;
    }

    table_[idx] = NULL;
    segmentState.parts.numHoles++;
    //LOG(INFO) << "+++ left hole at " << idx << ", holes=" << segmentState.parts.numHoles;
  }

  return true;
}

std::ostream& operator<<(std::ostream& os, IndirectRefKind rhs) {
  switch (rhs) {
  case kSirtOrInvalid:
    os << "stack indirect reference table or invalid reference";
    break;
  case kLocal:
    os << "local reference";
    break;
  case kGlobal:
    os << "global reference";
    break;
  case kWeakGlobal:
    os << "weak global reference";
    break;
  default:
    os << "IndirectRefKind[" << static_cast<int>(rhs) << "]";
    break;
  }
  return os;
}

void IndirectReferenceTable::Dump() const {
  LOG(WARNING) << kind_ << " table dump:";
  std::vector<const Object*> entries(table_, table_ + Capacity());
  ReferenceTable::Dump(entries);
}

}  // namespace art
