/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_RUNTIME_NATIVE_BRIDGE_ART_INTERFACE_H_
#define ART_RUNTIME_NATIVE_BRIDGE_ART_INTERFACE_H_

#include <jni.h>
#include <stdint.h>

namespace art {

const char* GetMethodShorty(JNIEnv* env, jmethodID mid);

uint32_t GetNativeMethodCount(JNIEnv* env, jclass clazz);

uint32_t GetNativeMethods(JNIEnv* env, jclass clazz, JNINativeMethod* methods,
                          uint32_t method_count);

};  // namespace art

#endif  // ART_RUNTIME_NATIVE_BRIDGE_ART_INTERFACE_H_
