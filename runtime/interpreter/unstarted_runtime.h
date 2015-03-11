/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_RUNTIME_INTERPRETER_UNSTARTED_RUNTIME_H_
#define ART_RUNTIME_INTERPRETER_UNSTARTED_RUNTIME_H_

#include "interpreter.h"

#include "dex_file.h"
#include "jvalue.h"

namespace art {

class Thread;
class ShadowFrame;

namespace mirror {

class ArtMethod;
class Object;

}  // namespace mirror

namespace interpreter {

void UnstartedRuntimeInitialize();

void UnstartedRuntimeInvoke(Thread* self, const DexFile::CodeItem* code_item,
                            ShadowFrame* shadow_frame,
                            JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void UnstartedRuntimeJni(Thread* self, mirror::ArtMethod* method, mirror::Object* receiver,
                         uint32_t* args, JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

}  // namespace interpreter
}  // namespace art

#endif  // ART_RUNTIME_INTERPRETER_UNSTARTED_RUNTIME_H_
