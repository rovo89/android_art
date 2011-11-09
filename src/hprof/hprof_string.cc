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
/*
 * Common string pool for the profiler
 */
#include "hprof.h"
#include "object.h"
#include "unordered_map.h"
#include "logging.h"

namespace art {

namespace hprof {

HprofStringId Hprof::LookupStringId(String* string) {
    return LookupStringId(string->ToModifiedUtf8());
}

HprofStringId Hprof::LookupStringId(const char* string) {
    return LookupStringId(std::string(string));
}

HprofStringId Hprof::LookupStringId(std::string string) {
    MutexLock mu(strings_lock_);
    if (strings_.find(string) == strings_.end()) {
        strings_[string] = next_string_id_++;
    }
    return strings_[string];
}

int Hprof::DumpStrings() {
    MutexLock mu(strings_lock_);

    HprofRecord *rec = &current_record_;

    for (StringMapIterator it = strings_.begin(); it != strings_.end(); ++it) {
        std::string string = (*it).first;
        size_t id = (*it).second;

        int err = StartNewRecord(HPROF_TAG_STRING, HPROF_TIME);
        if (err != 0) {
            return err;
        }

        /* STRING format:
         *
         * ID:     ID for this string
         * [uint8_t]*:  UTF8 characters for string (NOT NULL terminated)
         *         (the record format encodes the length)
         *
         * We use the address of the string data as its ID.
         */
        err = rec->AddU4(id);
        if (err != 0) {
            return err;
        }
        err = rec->AddUtf8String(string.c_str());
        if (err != 0) {
            return err;
        }
    }

    return 0;
}

}  // namespace hprof

}  // namespace art
