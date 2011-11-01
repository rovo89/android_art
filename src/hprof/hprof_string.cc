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
#include "unordered_set.h"
#include "logging.h"

namespace art {

namespace hprof {

typedef std::tr1::unordered_set<String*, StringHashCode> StringSet;
typedef std::tr1::unordered_set<String*, StringHashCode>::iterator StringSetIterator; // TODO: equals by VALUE not REFERENCE
static Mutex strings_lock_("hprof strings");
static StringSet strings_;

int hprofStartup_String() {
    return 0;
}

int hprofShutdown_String() {
    return 0;
}

hprof_string_id hprofLookupStringId(String* string) {
    MutexLock mu(strings_lock_);
    std::pair<StringSetIterator, bool> result = strings_.insert(string);
    String* present = *result.first;
    return (hprof_string_id) present;
}

hprof_string_id hprofLookupStringId(const char* string) {
    return hprofLookupStringId(String::AllocFromModifiedUtf8(string)); // TODO: leaks? causes GC?
}

hprof_string_id hprofLookupStringId(std::string string) {
    return hprofLookupStringId(string.c_str()); // TODO: leaks? causes GC?
}

int hprofDumpStrings(hprof_context_t *ctx) {
    MutexLock mu(strings_lock_);

    hprof_record_t *rec = &ctx->curRec;

    for (StringSetIterator it = strings_.begin(); it != strings_.end(); ++it) {
        String* string = *it;
        CHECK(string != NULL);

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
        err = hprofAddU4ToRecord(rec, (uint32_t) string);
        if (err != 0) {
            return err;
        }
        err = hprofAddUtf8StringToRecord(rec, string->ToModifiedUtf8().c_str()); // TODO: leak?
        if (err != 0) {
            return err;
        }
    }

    return 0;
}

}  // namespace hprof

}  // namespace art
