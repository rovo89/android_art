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
 * Class object pool
 */

#include "hprof.h"
#include "object.h"
#include "logging.h"
#include "unordered_set.h"

namespace art {

namespace hprof {

typedef std::tr1::unordered_set<Class*, ObjectIdentityHash> ClassSet;
typedef std::tr1::unordered_set<Class*, ObjectIdentityHash>::iterator ClassSetIterator;
static Mutex classes_lock_("hprof classes");
static ClassSet classes_;

int hprofStartup_Class() {
    return 0;
}

int hprofShutdown_Class() {
    return 0;
}

static int getPrettyClassNameId(Class* clazz) {
    std::string name(PrettyClass(clazz));
    return hprofLookupStringId(name); // TODO: leaks
}

hprof_class_object_id hprofLookupClassId(Class* clazz) {
    if (clazz == NULL) {
        /* Someone's probably looking up the superclass
         * of java.lang.Object or of a primitive class.
         */
        return (hprof_class_object_id)0;
    }

    MutexLock mu(classes_lock_);

    std::pair<ClassSetIterator, bool> result = classes_.insert(clazz);
    Class* present = *result.first;

    /* Make sure that the class's name is in the string table.
     * This is a bunch of extra work that we only have to do
     * because of the order of tables in the output file
     * (strings need to be dumped before classes).
     */
    getPrettyClassNameId(clazz);

    return (hprof_string_id) present;
}

int hprofDumpClasses(hprof_context_t *ctx) {
    MutexLock mu(classes_lock_);

    hprof_record_t *rec = &ctx->curRec;

    uint32_t nextSerialNumber = 1;

    for (ClassSetIterator it = classes_.begin(); it != classes_.end(); ++it) {
        Class* clazz = *it;
        CHECK(clazz != NULL);

        int err = hprofStartNewRecord(ctx, HPROF_TAG_LOAD_CLASS, HPROF_TIME);
        if (err != 0) {
            return err;
        }

        /* LOAD CLASS format:
         *
         * uint32_t:     class serial number (always > 0)
         * ID:     class object ID
         * uint32_t:     stack trace serial number
         * ID:     class name string ID
         *
         * We use the address of the class object structure as its ID.
         */
        hprofAddU4ToRecord(rec, nextSerialNumber++);
        hprofAddIdToRecord(rec, (hprof_class_object_id) clazz);
        hprofAddU4ToRecord(rec, HPROF_NULL_STACK_TRACE);
        hprofAddIdToRecord(rec, getPrettyClassNameId(clazz));
    }

    return 0;
}

}  // namespace hprof

}  // namespace art
