/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "reference_table.h"

#include "indirect_reference_table.h"

#include "object.h"

namespace art {

ReferenceTable::ReferenceTable(const char* name,
    size_t initial_size, size_t max_size)
        : name_(name), max_size_(max_size) {
  CHECK_LE(initial_size, max_size);
  entries_.reserve(initial_size);
}

void ReferenceTable::Add(Object* obj) {
  DCHECK(obj != NULL);
  if (entries_.size() == max_size_) {
    LOG(FATAL) << "ReferenceTable '" << name_ << "' "
               << "overflowed (" << max_size_ << " entries)";
  }
  entries_.push_back(obj);
}

void ReferenceTable::Remove(Object* obj) {
  // We iterate backwards on the assumption that references are LIFO.
  for (int i = entries_.size() - 1; i >= 0; --i) {
    if (entries_[i] == obj) {
      entries_.erase(entries_.begin() + i);
      return;
    }
  }
}

// If "obj" is an array, return the number of elements in the array.
// Otherwise, return zero.
size_t GetElementCount(const Object* obj) {
  if (obj == NULL || obj == kClearedJniWeakGlobal || !obj->IsArray()) {
    return 0;
  }
  return obj->AsArray()->GetLength();
}

struct ObjectComparator {
  bool operator()(Object* obj1, Object* obj2){
    // Ensure null references and cleared jweaks appear at the end.
    if (obj1 == NULL) {
      return true;
    } else if (obj2 == NULL) {
      return false;
    }
    if (obj1 == kClearedJniWeakGlobal) {
      return true;
    } else if (obj2 == kClearedJniWeakGlobal) {
      return false;
    }

    // Sort by class...
    if (obj1->GetClass() != obj2->GetClass()) {
      return reinterpret_cast<uintptr_t>(obj1->GetClass()) <
          reinterpret_cast<uintptr_t>(obj2->GetClass());
    } else {
      // ...then by size...
      size_t count1 = obj1->SizeOf();
      size_t count2 = obj2->SizeOf();
      if (count1 != count2) {
        return count1 < count2;
      } else {
        // ...and finally by address.
        return reinterpret_cast<uintptr_t>(obj1) <
            reinterpret_cast<uintptr_t>(obj2);
      }
    }
  }
};

// Log an object with some additional info.
//
// Pass in the number of elements in the array (or 0 if this is not an
// array object), and the number of additional objects that are identical
// or equivalent to the original.
void LogSummaryLine(const Object* obj, size_t elems, int identical, int equiv) {
  if (obj == NULL) {
    LOG(WARNING) << "    NULL reference (count=" << equiv << ")";
    return;
  }
  if (obj == kClearedJniWeakGlobal) {
    LOG(WARNING) << "    cleared jweak (count=" << equiv << ")";
    return;
  }

  std::string className(PrettyType(obj));
  if (obj->IsClass()) {
    // We're summarizing multiple instances, so using the exemplar
    // Class' type parameter here would be misleading.
    className = "java.lang.Class";
  }
  if (elems != 0) {
    StringAppendF(&className, " (%zd elements)", elems);
  }

  size_t total = identical + equiv + 1;
  std::string msg(StringPrintf("%5d of %s", total, className.c_str()));
  if (identical + equiv != 0) {
    StringAppendF(&msg, " (%d unique instances)", equiv + 1);
  }
  LOG(WARNING) << "    " << msg;
}

size_t ReferenceTable::Size() const {
  return entries_.size();
}

void ReferenceTable::Dump() const {
  LOG(WARNING) << name_ << " reference table dump:";
  Dump(entries_);
}

void ReferenceTable::Dump(const std::vector<Object*>& entries) {
  if (entries.empty()) {
    LOG(WARNING) << "  (empty)";
    return;
  }

  // Dump the most recent N entries.
  const size_t kLast = 10;
  size_t count = entries.size();
  int first = count - kLast;
  if (first < 0) {
    first = 0;
  }
  LOG(WARNING) << "  Last " << (count - first) << " entries (of " << count << "):";
  for (int idx = count - 1; idx >= first; --idx) {
    const Object* ref = entries[idx];
    if (ref == NULL) {
      continue;
    }
    if (ref == kClearedJniWeakGlobal) {
      LOG(WARNING) << StringPrintf("    %5d: cleared jweak", idx);
      continue;
    }
    if (ref->GetClass() == NULL) {
      // should only be possible right after a plain dvmMalloc().
      size_t size = ref->SizeOf();
      LOG(WARNING) << StringPrintf("    %5d: %p (raw) (%zd bytes)", idx, ref, size);
      continue;
    }

    std::string className(PrettyType(ref));

    std::string extras;
    size_t elems = GetElementCount(ref);
    if (elems != 0) {
      StringAppendF(&extras, " (%zd elements)", elems);
    }
#if 0
    // TODO: support dumping string data.
    else if (ref->GetClass() == gDvm.classJavaLangString) {
      const StringObject* str = reinterpret_cast<const StringObject*>(ref);
      extras += " \"";
      size_t count = 0;
      char* s = dvmCreateCstrFromString(str);
      char* p = s;
      for (; *p && count < 16; ++p, ++count) {
        extras += *p;
      }
      if (*p == 0) {
        extras += "\"";
      } else {
        StringAppendF(&extras, "... (%d chars)", str->length());
      }
      free(s);
    }
#endif
    LOG(WARNING) << StringPrintf("    %5d: ", idx) << ref << " " << className << extras;
  }

  // Make a copy of the table and sort it.
  std::vector<Object*> sorted_entries(entries.begin(), entries.end());
  std::sort(sorted_entries.begin(), sorted_entries.end(), ObjectComparator());

  // Remove any uninteresting stuff from the list. The sort moved them all to the end.
  while (!sorted_entries.empty() && sorted_entries.back() == NULL) {
    sorted_entries.pop_back();
  }
  while (!sorted_entries.empty() && sorted_entries.back() == kClearedJniWeakGlobal) {
    sorted_entries.pop_back();
  }
  if (sorted_entries.empty()) {
    return;
  }

  // Dump a summary of the whole table.
  LOG(WARNING) << "  Summary:";
  size_t equiv = 0;
  size_t identical = 0;
  for (size_t idx = 1; idx < count; idx++) {
    Object* prev = sorted_entries[idx-1];
    Object* current = sorted_entries[idx];
    size_t elems = GetElementCount(prev);
    if (current == prev) {
      // Same reference, added more than once.
      identical++;
    } else if (current->GetClass() == prev->GetClass() && GetElementCount(current) == elems) {
      // Same class / element count, different object.
      equiv++;
    } else {
      // Different class.
      LogSummaryLine(prev, elems, identical, equiv);
      equiv = identical = 0;
    }
  }
  // Handle the last entry.
  LogSummaryLine(sorted_entries.back(), GetElementCount(sorted_entries.back()), identical, equiv);
}

}  // namespace art
