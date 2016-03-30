/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ART_COMPILER_UTILS_STRING_REFERENCE_H_
#define ART_COMPILER_UTILS_STRING_REFERENCE_H_

#include <stdint.h>

#include "base/logging.h"
#include "utf-inl.h"

namespace art {

class DexFile;

// A string is uniquely located by its DexFile and the string_ids_ table index into that DexFile.
struct StringReference {
  StringReference(const DexFile* file, uint32_t index) : dex_file(file), string_index(index) { }

  const DexFile* dex_file;
  uint32_t string_index;
};

// Compare the actual referenced string values. Used for string reference deduplication.
struct StringReferenceValueComparator {
  bool operator()(StringReference sr1, StringReference sr2) const {
    // Note that we want to deduplicate identical strings even if they are referenced
    // by different dex files, so we need some (any) total ordering of strings, rather
    // than references. However, the references should usually be from the same dex file,
    // so we choose the dex file string ordering so that we can simply compare indexes
    // and avoid the costly string comparison in the most common case.
    if (sr1.dex_file == sr2.dex_file) {
      // Use the string order enforced by the dex file verifier.
      DCHECK_EQ(
          sr1.string_index < sr2.string_index,
          CompareModifiedUtf8ToModifiedUtf8AsUtf16CodePointValues(
              sr1.dex_file->GetStringData(sr1.dex_file->GetStringId(sr1.string_index)),
              sr1.dex_file->GetStringData(sr2.dex_file->GetStringId(sr2.string_index))) < 0);
      return sr1.string_index < sr2.string_index;
    } else {
      // Cannot compare indexes, so do the string comparison.
      return CompareModifiedUtf8ToModifiedUtf8AsUtf16CodePointValues(
          sr1.dex_file->GetStringData(sr1.dex_file->GetStringId(sr1.string_index)),
          sr1.dex_file->GetStringData(sr2.dex_file->GetStringId(sr2.string_index))) < 0;
    }
  }
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_STRING_REFERENCE_H_
