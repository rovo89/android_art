// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni.h"
#include <stdio.h>

extern "C"
JNIEXPORT jint JNICALL Java_MyClass_bar(JNIEnv* env, jobject thisObj, jint count) {
  return count + 1;
}
