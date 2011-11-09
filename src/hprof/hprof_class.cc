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

HprofStringId Hprof::LookupClassNameId(Class* clazz) {
    return LookupStringId(PrettyDescriptor(clazz->GetDescriptor()));
}

HprofClassObjectId Hprof::LookupClassId(Class* clazz) {
    if (clazz == NULL) {
        /* Someone's probably looking up the superclass
         * of java.lang.Object or of a primitive class.
         */
        return (HprofClassObjectId)0;
    }

    MutexLock mu(classes_lock_);

    std::pair<ClassSetIterator, bool> result = classes_.insert(clazz);
    Class* present = *result.first;

    // Make sure that we've assigned a string ID for this class' name
    LookupClassNameId(clazz);

    CHECK_EQ(present, clazz);
    return (HprofStringId) present;
}

int Hprof::DumpClasses() {
    MutexLock mu(classes_lock_);

    HprofRecord *rec = &current_record_;

    uint32_t nextSerialNumber = 1;

    for (ClassSetIterator it = classes_.begin(); it != classes_.end(); ++it) {
        Class* clazz = *it;
        CHECK(clazz != NULL);

        int err = StartNewRecord(HPROF_TAG_LOAD_CLASS, HPROF_TIME);
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
        rec->AddU4(nextSerialNumber++);
        rec->AddId((HprofClassObjectId) clazz);
        rec->AddU4(HPROF_NULL_STACK_TRACE);
        rec->AddId(LookupClassNameId(clazz));
    }

    return 0;
}

}  // namespace hprof

}  // namespace art
