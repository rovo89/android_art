// Copyright 2011 Google Inc. All Rights Reserved.

#include <signal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "ScopedLocalRef.h"
#include "UniquePtr.h"
#include "jni.h"
#include "logging.h"
#include "toStringArray.h"

// Determine whether or not the specified method is public.
static bool IsMethodPublic(JNIEnv* env, jclass clazz, jmethodID method_id) {
  ScopedLocalRef<jobject> reflected(env, env->ToReflectedMethod(clazz, method_id, JNI_FALSE));
  if (reflected.get() == NULL) {
    fprintf(stderr, "Unable to get reflected method\n");
    return false;
  }
  // We now have a Method instance.  We need to call its
  // getModifiers() method.
  ScopedLocalRef<jclass> method(env, env->FindClass("java/lang/reflect/Method"));
  if (method.get() == NULL) {
    fprintf(stderr, "Unable to find class Method\n");
    return false;
  }
  static const int PUBLIC = 0x0001;   // java.lang.reflect.Modifiers.PUBLIC
  jmethodID get_modifiers = env->GetMethodID(method.get(), "getModifiers", "()I");
  if (get_modifiers == NULL) {
    fprintf(stderr, "Unable to find reflect.Method.getModifiers\n");
    return false;
  }
  int modifiers = env->CallIntMethod(reflected.get(), get_modifiers);
  if ((modifiers & PUBLIC) == 0) {
    return false;
  }
  return true;
}

static int InvokeMain(JNIEnv* env, int argc, char** argv) {
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

// Parse arguments.  Most of it just gets passed through to the VM.
// The JNI spec defines a handful of standard arguments.
int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);

  // Skip over argv[0].
  argv++;
  argc--;

  // If we're adding any additional stuff, e.g. function hook specifiers,
  // add them to the count here.
  //
  // We're over-allocating, because this includes the options to the VM
  // plus the options to the program.
  int option_count = argc;
  UniquePtr<JavaVMOption[]> options(new JavaVMOption[option_count]());

  // Copy options over.  Everything up to the name of the class starts
  // with a '-' (the function hook stuff is strictly internal).
  //
  // [Do we need to catch & handle "-jar" here?]
  bool need_extra = false;
  int curr_opt, arg_idx;
  for (curr_opt = arg_idx = 0; arg_idx < argc; arg_idx++) {
    if (argv[arg_idx][0] != '-' && !need_extra) {
      break;
    }
    options[curr_opt++].optionString = argv[arg_idx];

    // Some options require an additional argument.
    need_extra = false;
    if (strcmp(argv[arg_idx], "-classpath") == 0 || strcmp(argv[arg_idx], "-cp") == 0) {
      // others?
      need_extra = true;
    }
  }

  if (need_extra) {
    fprintf(stderr, "VM requires value after last option flag\n");
    return EXIT_FAILURE;
  }

  // Make sure they provided a class name.  We do this after VM init
  // so that things like "-Xrunjdwp:help" have the opportunity to emit
  // a usage statement.
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

  // Start VM.  The current thread becomes the main thread of the VM.
  JavaVM* vm = NULL;
  JNIEnv* env = NULL;
  if (JNI_CreateJavaVM(&vm, &env, &init_args) != JNI_OK) {
    fprintf(stderr, "VM init failed (check log file)\n");
    return EXIT_FAILURE;
  }

  int rc = InvokeMain(env, argc - arg_idx, &argv[arg_idx]);

  if (vm->DetachCurrentThread() != JNI_OK) {
    fprintf(stderr, "Warning: unable to detach main thread\n");
    rc = EXIT_FAILURE;
  }

  if (vm->DestroyJavaVM() != 0) {
    fprintf(stderr, "Warning: VM did not shut down cleanly\n");
    rc = EXIT_FAILURE;
  }

  return rc;
}
