/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <signal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "jni.h"
#include "logging.h"
#include "object.h"
#include "ScopedLocalRef.h"
#include "toStringArray.h"
#include "UniquePtr.h"
#include "well_known_classes.h"

namespace art {

// Determine whether or not the specified method is public.
static bool IsMethodPublic(JNIEnv* env, jclass c, jmethodID method_id) {
  ScopedLocalRef<jobject> reflected(env, env->ToReflectedMethod(c, method_id, JNI_FALSE));
  if (reflected.get() == NULL) {
    fprintf(stderr, "Failed to get reflected method\n");
    return false;
  }
  // We now have a Method instance.  We need to call its
  // getModifiers() method.
  jmethodID mid = env->GetMethodID(WellKnownClasses::java_lang_reflect_Method, "getModifiers", "()I");
  if (mid == NULL) {
    fprintf(stderr, "Failed to find java.lang.reflect.Method.getModifiers\n");
    return false;
  }
  int modifiers = env->CallIntMethod(reflected.get(), mid);
  if ((modifiers & kAccPublic) == 0) {
    return false;
  }
  return true;
}

static int InvokeMain(JNIEnv* env, char** argv) {
  // We want to call main() with a String array with our arguments in
  // it.  Create an array and populate it.  Note argv[0] is not
  // included.
  ScopedLocalRef<jobjectArray> args(env, toStringArray(env, argv + 1));
  if (args.get() == NULL) {
    env->ExceptionDescribe();
    return EXIT_FAILURE;
  }

  // Find [class].main(String[]).

  // Convert "com.android.Blah" to "com/android/Blah".
  std::string class_name(argv[0]);
  std::replace(class_name.begin(), class_name.end(), '.', '/');

  ScopedLocalRef<jclass> klass(env, env->FindClass(class_name.c_str()));
  if (klass.get() == NULL) {
    fprintf(stderr, "Unable to locate class '%s'\n", class_name.c_str());
    env->ExceptionDescribe();
    return EXIT_FAILURE;
  }

  jmethodID method = env->GetStaticMethodID(klass.get(), "main", "([Ljava/lang/String;)V");
  if (method == NULL) {
    fprintf(stderr, "Unable to find static main(String[]) in '%s'\n", class_name.c_str());
    env->ExceptionDescribe();
    return EXIT_FAILURE;
  }

  // Make sure the method is public.  JNI doesn't prevent us from
  // calling a private method, so we have to check it explicitly.
  if (!IsMethodPublic(env, klass.get(), method)) {
    fprintf(stderr, "Sorry, main() is not public in '%s'\n", class_name.c_str());
    env->ExceptionDescribe();
    return EXIT_FAILURE;
  }

  // Invoke main().
  env->CallStaticVoidMethod(klass.get(), method, args.get());

  // Check whether there was an uncaught exception. We don't log any uncaught exception here;
  // detaching this thread will do that for us, but it will clear the exception (and invalidate
  // our JNIEnv), so we need to check here.
  return env->ExceptionCheck() ? EXIT_FAILURE : EXIT_SUCCESS;
}

// Parse arguments.  Most of it just gets passed through to the runtime.
// The JNI spec defines a handful of standard arguments.
static int oatexec(int argc, char** argv) {
  InitLogging();
  setvbuf(stdout, NULL, _IONBF, 0);

  // Skip over argv[0].
  argv++;
  argc--;

  // If we're adding any additional stuff, e.g. function hook specifiers,
  // add them to the count here.
  //
  // We're over-allocating, because this includes the options to the runtime
  // plus the options to the program.
  int option_count = argc;
  UniquePtr<JavaVMOption[]> options(new JavaVMOption[option_count]());

  // Copy options over.  Everything up to the name of the class starts
  // with a '-' (the function hook stuff is strictly internal).
  //
  // [Do we need to catch & handle "-jar" here?]
  bool need_extra = false;
  const char* what = NULL;
  int curr_opt, arg_idx;
  for (curr_opt = arg_idx = 0; arg_idx < argc; arg_idx++) {
    if (argv[arg_idx][0] != '-' && !need_extra) {
      break;
    }
    options[curr_opt++].optionString = argv[arg_idx];

    // Some options require an additional argument.
    need_extra = false;
    if (strcmp(argv[arg_idx], "-classpath") == 0 || strcmp(argv[arg_idx], "-cp") == 0) {
      need_extra = true;
      what = argv[arg_idx];
    }
  }

  if (need_extra) {
    fprintf(stderr, "%s must be followed by an additional argument giving a value\n", what);
    return EXIT_FAILURE;
  }

  // Make sure they provided a class name.
  if (arg_idx == argc) {
    fprintf(stderr, "Class name required\n");
    return EXIT_FAILURE;
  }

  // insert additional internal options here

  DCHECK_LE(curr_opt, option_count);

  JavaVMInitArgs init_args;
  init_args.version = JNI_VERSION_1_6;
  init_args.options = options.get();
  init_args.nOptions = curr_opt;
  init_args.ignoreUnrecognized = JNI_FALSE;

  // Start the runtime. The current thread becomes the main thread.
  JavaVM* vm = NULL;
  JNIEnv* env = NULL;
  if (JNI_CreateJavaVM(&vm, &env, &init_args) != JNI_OK) {
    fprintf(stderr, "runtime failed to initialize (check log for details)\n");
    return EXIT_FAILURE;
  }

  int rc = InvokeMain(env, &argv[arg_idx]);

#if defined(NDEBUG)
  // The DestroyJavaVM call will detach this thread for us. In debug builds, we don't want to
  // detach because detaching disables the CheckSafeToLockOrUnlock checking.
  if (vm->DetachCurrentThread() != JNI_OK) {
    fprintf(stderr, "Warning: unable to detach main thread\n");
    rc = EXIT_FAILURE;
  }
#endif

  if (vm->DestroyJavaVM() != 0) {
    fprintf(stderr, "Warning: runtime did not shut down cleanly\n");
    rc = EXIT_FAILURE;
  }

  return rc;
}

} // namespace art

int main(int argc, char** argv) {
  return art::oatexec(argc, argv);
}
