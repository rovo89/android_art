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

#include "jni_internal.h"
#include "JNIHelp.h"
#include "ScopedUtfChars.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

#include <paths.h>
#include <stdlib.h>
#include <unistd.h>

namespace art {

namespace {

void Zygote_nativeExecShell(JNIEnv* env, jclass, jstring javaCommand) {
  ScopedUtfChars command(env, javaCommand);
  if (command.c_str() == NULL) {
    return;
  }
  const char *argp[] = {_PATH_BSHELL, "-c", command.c_str(), NULL};
  LOG(INFO) << "Exec: " << argp[0] << ' ' << argp[1] << ' ' << argp[2];

  execv(_PATH_BSHELL, (char**)argp);
  exit(127);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Zygote, nativeExecShell, "(Ljava/lang/String;)V"),
  //NATIVE_METHOD(Zygote, nativeFork, "()I"),
  //NATIVE_METHOD(Zygote, nativeForkAndSpecialize, "(II[II[[I)I"),
  //NATIVE_METHOD(Zygote, nativeForkSystemServer, "(II[II[[IJJ)I"),
};

}  // namespace

void register_dalvik_system_Zygote(JNIEnv* env) {
  jniRegisterNativeMethods(env, "dalvik/system/Zygote", gMethods, NELEM(gMethods));
}

}  // namespace art
