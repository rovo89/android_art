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

size_t next_string_id_ = 0x400000;
typedef std::tr1::unordered_map<std::string, size_t> StringMap;
typedef std::tr1::unordered_map<std::string, size_t>::iterator StringMapIterator;
static Mutex strings_lock_("hprof strings");
static StringMap strings_;

int hprofStartup_String() {
    return 0;
}

int hprofShutdown_String() {
    return 0;
}

hprof_string_id hprofLookupStringId(String* string) {
    return hprofLookupStringId(string->ToModifiedUtf8());
}

hprof_string_id hprofLookupStringId(const char* string) {
    return hprofLookupStringId(std::string(string));
}

hprof_string_id hprofLookupStringId(std::string string) {
    MutexLock mu(strings_lock_);
    if (strings_.find(string) == strings_.end()) {
        strings_[string] = next_string_id_++;
    }
    return strings_[string];
}

int hprofDumpStrings(hprof_context_t *ctx) {
    MutexLock mu(strings_lock_);

    hprof_record_t *rec = &ctx->curRec;

    for (StringMapIterator it = strings_.begin(); it != strings_.end(); ++it) {
        std::string string = (*it).first;
        size_t id = (*it).second;

        int err = hprofStartNewRecord(ctx, HPROF_TAG_STRING, HPROF_TIME);
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
        err = hprofAddU4ToRecord(rec, id);
        if (err != 0) {
            return err;
        }
        err = hprofAddUtf8StringToRecord(rec, string.c_str());
        if (err != 0) {
            return err;
        }
    }

    return 0;
}

}  // namespace hprof

}  // namespace art
