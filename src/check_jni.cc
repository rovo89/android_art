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

#include <sys/mman.h>
#include <zlib.h>

#include "class_linker.h"
#include "logging.h"
#include "scoped_jni_thread_state.h"
#include "thread.h"
#include "runtime.h"

namespace art {

void JniAbort(const char* jni_function_name) {
  Thread* self = Thread::Current();
  const Method* current_method = self->GetCurrentMethod();

  std::stringstream os;
  os << "JNI app bug detected";

  if (jni_function_name != NULL) {
    os << "\n             in call to " << jni_function_name;
  }
  // TODO: is this useful given that we're about to dump the calling thread's stack?
  if (current_method != NULL) {
    os << "\n             from " << PrettyMethod(current_method);
  }
  os << "\n";
  self->Dump(os);

  JavaVMExt* vm = Runtime::Current()->GetJavaVM();
  if (vm->check_jni_abort_hook != NULL) {
    vm->check_jni_abort_hook(os.str());
  } else {
    LOG(FATAL) << os.str();
  }
}

/*
 * ===========================================================================
 *      JNI function helpers
 * ===========================================================================
 */

template<typename T>
T Decode(ScopedJniThreadState& ts, jobject obj) {
  return reinterpret_cast<T>(ts.Self()->DecodeJObject(obj));
}

/*
 * Hack to allow forcecopy to work with jniGetNonMovableArrayElements.
 * The code deliberately uses an invalid sequence of operations, so we
 * need to pass it through unmodified.  Review that code before making
 * any changes here.
 */
#define kNoCopyMagic    0xd5aab57f

/*
 * Flags passed into ScopedCheck.
 */
#define kFlag_Default       0x0000

#define kFlag_CritBad       0x0000      /* calling while in critical is bad */
#define kFlag_CritOkay      0x0001      /* ...okay */
#define kFlag_CritGet       0x0002      /* this is a critical "get" */
#define kFlag_CritRelease   0x0003      /* this is a critical "release" */
#define kFlag_CritMask      0x0003      /* bit mask to get "crit" value */

#define kFlag_ExcepBad      0x0000      /* raised exceptions are bad */
#define kFlag_ExcepOkay     0x0004      /* ...okay */

#define kFlag_Release       0x0010      /* are we in a non-critical release function? */
#define kFlag_NullableUtf   0x0020      /* are our UTF parameters nullable? */

#define kFlag_Invocation    0x8000      /* Part of the invocation interface (JavaVM*) */

static const char* gBuiltInPrefixes[] = {
  "Landroid/",
  "Lcom/android/",
  "Lcom/google/android/",
  "Ldalvik/",
  "Ljava/",
  "Ljavax/",
  "Llibcore/",
  "Lorg/apache/harmony/",
  NULL
};

bool ShouldTrace(JavaVMExt* vm, const Method* method) {
  // If both "-Xcheck:jni" and "-Xjnitrace:" are enabled, we print trace messages
  // when a native method that matches the -Xjnitrace argument calls a JNI function
  // such as NewByteArray.
  // If -verbose:third-party-jni is on, we want to log any JNI function calls
  // made by a third-party native method.
  std::string classNameStr(method->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8());
  if (!vm->trace.empty() && classNameStr.find(vm->trace) != std::string::npos) {
    return true;
  }
  if (vm->log_third_party_jni) {
    // Return true if we're trying to log all third-party JNI activity and 'method' doesn't look
    // like part of Android.
    StringPiece className(classNameStr);
    for (size_t i = 0; gBuiltInPrefixes[i] != NULL; ++i) {
      if (className.starts_with(gBuiltInPrefixes[i])) {
        return false;
      }
    }
    return true;
  }
  return false;
}

class ScopedCheck {
public:
  // For JNIEnv* functions.
  explicit ScopedCheck(JNIEnv* env, int flags, const char* functionName) {
    init(env, reinterpret_cast<JNIEnvExt*>(env)->vm, flags, functionName, true);
    checkThread(flags);
  }

  // For JavaVM* functions.
  explicit ScopedCheck(JavaVM* vm, bool hasMethod, const char* functionName) {
    init(NULL, vm, kFlag_Invocation, functionName, hasMethod);
  }

  bool forceCopy() {
    return Runtime::Current()->GetJavaVM()->force_copy;
  }

  /*
   * In some circumstances the VM will screen class names, but it doesn't
   * for class lookup.  When things get bounced through a class loader, they
   * can actually get normalized a couple of times; as a result, passing in
   * a class name like "java.lang.Thread" instead of "java/lang/Thread" will
   * work in some circumstances.
   *
   * This is incorrect and could cause strange behavior or compatibility
   * problems, so we want to screen that out here.
   *
   * We expect "fully-qualified" class names, like "java/lang/Thread" or
   * "[Ljava/lang/Object;".
   */
  void checkClassName(const char* className) {
    if (!IsValidClassName(className, true, false)) {
      LOG(ERROR) << "JNI ERROR: illegal class name '" << className << "' (" << mFunctionName << ")\n"
                 << "           (should be of the form 'java/lang/String', [Ljava/lang/String;' or '[[B')\n";
      JniAbort();
    }
  }

  /*
   * Verify that the field is of the appropriate type.  If the field has an
   * object type, "java_object" is the object we're trying to assign into it.
   *
   * Works for both static and instance fields.
   */
  void checkFieldType(jobject java_object, jfieldID fid, char prim, bool isStatic) {
    if (fid == NULL) {
      LOG(ERROR) << "JNI ERROR: null jfieldID";
      JniAbort();
      return;
    }

    ScopedJniThreadState ts(mEnv);
    Field* f = DecodeField(fid);
    Class* field_type = f->GetType();
    if (!field_type->IsPrimitive()) {
      if (java_object != NULL) {
        Object* obj = Decode<Object*>(ts, java_object);
        /*
         * If java_object is a weak global ref whose referent has been cleared,
         * obj will be NULL.  Otherwise, obj should always be non-NULL
         * and valid.
         */
        if (obj != NULL && !Heap::IsHeapAddress(obj)) {
          LOG(ERROR) << "JNI ERROR: field operation on invalid " << GetIndirectRefKind(java_object) << ": " << java_object;
          JniAbort();
          return;
        } else {
          if (!obj->InstanceOf(field_type)) {
            LOG(ERROR) << "JNI ERROR: attempt to set field " << PrettyField(f) << " with value of wrong type: " << PrettyTypeOf(obj);
            JniAbort();
            return;
          }
        }
      }
    } else if (field_type != Runtime::Current()->GetClassLinker()->FindPrimitiveClass(prim)) {
      LOG(ERROR) << "JNI ERROR: attempt to set field " << PrettyField(f) << " with value of wrong type: " << prim;
      JniAbort();
      return;
    }

    if (isStatic && !f->IsStatic()) {
      if (isStatic) {
        LOG(ERROR) << "JNI ERROR: accessing non-static field " << PrettyField(f) << " as static";
      } else {
        LOG(ERROR) << "JNI ERROR: accessing static field " << PrettyField(f) << " as non-static";
      }
      JniAbort();
      return;
    }
  }

  /*
   * Verify that this instance field ID is valid for this object.
   *
   * Assumes "jobj" has already been validated.
   */
  void checkInstanceFieldID(jobject java_object, jfieldID fid) {
    ScopedJniThreadState ts(mEnv);

    Object* o = Decode<Object*>(ts, java_object);
    if (!Heap::IsHeapAddress(o)) {
      LOG(ERROR) << "JNI ERROR: field operation on invalid " << GetIndirectRefKind(java_object) << ": " << java_object;
      JniAbort();
      return;
    }

    Field* f = DecodeField(fid);
    Class* f_type = f->GetType();
    // check invariant that all jfieldIDs have resovled types
    DCHECK(f_type != NULL);
    Class* c = o->GetClass();
    if (c->FindInstanceField(f->GetName()->ToModifiedUtf8(), f_type) == NULL) {
      LOG(ERROR) << "JNI ERROR: jfieldID " << PrettyField(f) << " not valid for an object of class " << PrettyTypeOf(o);
      JniAbort();
    }
  }

  /*
   * Verify that the pointer value is non-NULL.
   */
  void checkNonNull(const void* ptr) {
    if (ptr == NULL) {
      LOG(ERROR) << "JNI ERROR: invalid null pointer";
      JniAbort();
    }
  }

  /*
   * Verify that the method's return type matches the type of call.
   * 'expectedType' will be "L" for all objects, including arrays.
   */
  void checkSig(jmethodID mid, const char* expectedType, bool isStatic) {
    ScopedJniThreadState ts(mEnv);
    const Method* m = DecodeMethod(mid);
    if (*expectedType != m->GetShorty()->CharAt(0)) {
      LOG(ERROR) << "JNI ERROR: expected return type '" << *expectedType << "' calling " << PrettyMethod(m);
      JniAbort();
    } else if (isStatic && !m->IsStatic()) {
      if (isStatic) {
        LOG(ERROR) << "JNI ERROR: calling non-static method " << PrettyMethod(m) << " with static call";
      } else {
        LOG(ERROR) << "JNI ERROR: calling static method " << PrettyMethod(m) << " with non-static call";
      }
      JniAbort();
    }
  }

  /*
   * Verify that this static field ID is valid for this class.
   *
   * Assumes "java_class" has already been validated.
   */
  void checkStaticFieldID(jclass java_class, jfieldID fid) {
    ScopedJniThreadState ts(mEnv);
    Class* c = Decode<Class*>(ts, java_class);
    const Field* f = DecodeField(fid);
    if (f->GetDeclaringClass() != c) {
      LOG(ERROR) << "JNI ERROR: static jfieldID " << fid << " not valid for class " << PrettyClass(c);
      JniAbort();
    }
  }

  /*
   * Verify that "mid" is appropriate for "clazz".
   *
   * A mismatch isn't dangerous, because the jmethodID defines the class.  In
   * fact, jclazz is unused in the implementation.  It's best if we don't
   * allow bad code in the system though.
   *
   * Instances of "jclazz" must be instances of the method's declaring class.
   */
  void checkStaticMethod(jclass java_class, jmethodID mid) {
    ScopedJniThreadState ts(mEnv);
    Class* c = Decode<Class*>(ts, java_class);
    const Method* m = DecodeMethod(mid);
    if (!c->IsAssignableFrom(m->GetDeclaringClass())) {
      LOG(ERROR) << "JNI ERROR: can't call static " << PrettyMethod(m) << " on class " << PrettyClass(c);
      JniAbort();
    }
  }

  /*
   * Verify that "mid" is appropriate for "jobj".
   *
   * Make sure the object is an instance of the method's declaring class.
   * (Note the mid might point to a declaration in an interface; this
   * will be handled automatically by the instanceof check.)
   */
  void checkVirtualMethod(jobject java_object, jmethodID mid) {
    ScopedJniThreadState ts(mEnv);
    Object* o = Decode<Object*>(ts, java_object);
    const Method* m = DecodeMethod(mid);
    if (!o->InstanceOf(m->GetDeclaringClass())) {
      LOG(ERROR) << "JNI ERROR: can't call " << PrettyMethod(m) << " on instance of " << PrettyTypeOf(o);
      JniAbort();
    }
  }

  /**
   * The format string is a sequence of the following characters,
   * and must be followed by arguments of the corresponding types
   * in the same order.
   *
   * Java primitive types:
   * B - jbyte
   * C - jchar
   * D - jdouble
   * F - jfloat
   * I - jint
   * J - jlong
   * S - jshort
   * Z - jboolean (shown as true and false)
   * V - void
   *
   * Java reference types:
   * L - jobject
   * a - jarray
   * c - jclass
   * s - jstring
   *
   * JNI types:
   * b - jboolean (shown as JNI_TRUE and JNI_FALSE)
   * f - jfieldID
   * m - jmethodID
   * p - void*
   * r - jint (for release mode arguments)
   * u - const char* (modified UTF-8)
   * z - jsize (for lengths; use i if negative values are okay)
   * v - JavaVM*
   * E - JNIEnv*
   * . - no argument; just print "..." (used for varargs JNI calls)
   *
   * Use the kFlag_NullableUtf flag where 'u' field(s) are nullable.
   */
  void check(bool entry, const char* fmt0, ...) {
    va_list ap;

    const Method* traceMethod = NULL;
    if ((!mVm->trace.empty() || mVm->log_third_party_jni) && mHasMethod) {
      // We need to guard some of the invocation interface's calls: a bad caller might
      // use DetachCurrentThread or GetEnv on a thread that's not yet attached.
      Thread* self = Thread::Current();
      if ((mFlags & kFlag_Invocation) == 0 || self != NULL) {
        traceMethod = self->GetCurrentMethod();
      }
    }

    if (traceMethod != NULL && ShouldTrace(mVm, traceMethod)) {
      va_start(ap, fmt0);
      std::string msg;
      for (const char* fmt = fmt0; *fmt;) {
        char ch = *fmt++;
        if (ch == 'B') { // jbyte
          jbyte b = va_arg(ap, int);
          if (b >= 0 && b < 10) {
            StringAppendF(&msg, "%d", b);
          } else {
            StringAppendF(&msg, "%#x (%d)", b, b);
          }
        } else if (ch == 'C') { // jchar
          jchar c = va_arg(ap, int);
          if (c < 0x7f && c >= ' ') {
            StringAppendF(&msg, "U+%x ('%c')", c, c);
          } else {
            StringAppendF(&msg, "U+%x", c);
          }
        } else if (ch == 'F' || ch == 'D') { // jfloat, jdouble
          StringAppendF(&msg, "%g", va_arg(ap, double));
        } else if (ch == 'I' || ch == 'S') { // jint, jshort
          StringAppendF(&msg, "%d", va_arg(ap, int));
        } else if (ch == 'J') { // jlong
          StringAppendF(&msg, "%lld", va_arg(ap, jlong));
        } else if (ch == 'Z') { // jboolean
          StringAppendF(&msg, "%s", va_arg(ap, int) ? "true" : "false");
        } else if (ch == 'V') { // void
          msg += "void";
        } else if (ch == 'v') { // JavaVM*
          JavaVM* vm = va_arg(ap, JavaVM*);
          StringAppendF(&msg, "(JavaVM*)%p", vm);
        } else if (ch == 'E') { // JNIEnv*
          JNIEnv* env = va_arg(ap, JNIEnv*);
          StringAppendF(&msg, "(JNIEnv*)%p", env);
        } else if (ch == 'L' || ch == 'a' || ch == 's') { // jobject, jarray, jstring
          // For logging purposes, these are identical.
          jobject o = va_arg(ap, jobject);
          if (o == NULL) {
            msg += "NULL";
          } else {
            StringAppendF(&msg, "%p", o);
          }
        } else if (ch == 'b') { // jboolean (JNI-style)
          jboolean b = va_arg(ap, int);
          msg += (b ? "JNI_TRUE" : "JNI_FALSE");
        } else if (ch == 'c') { // jclass
          jclass jc = va_arg(ap, jclass);
          Class* c = reinterpret_cast<Class*>(Thread::Current()->DecodeJObject(jc));
          if (c == NULL) {
            msg += "NULL";
          } else if (c == kInvalidIndirectRefObject || !Heap::IsHeapAddress(c)) {
            StringAppendF(&msg, "%p(INVALID)", jc);
          } else {
            msg += PrettyClass(c);
            if (!entry) {
              StringAppendF(&msg, " (%p)", jc);
            }
          }
        } else if (ch == 'f') { // jfieldID
          jfieldID fid = va_arg(ap, jfieldID);
          Field* f = reinterpret_cast<Field*>(fid);
          msg += PrettyField(f);
          if (!entry) {
            StringAppendF(&msg, " (%p)", fid);
          }
        } else if (ch == 'z') { // non-negative jsize
          // You might expect jsize to be size_t, but it's not; it's the same as jint.
          // We only treat this specially so we can do the non-negative check.
          // TODO: maybe this wasn't worth it?
          jint i = va_arg(ap, jint);
          StringAppendF(&msg, "%d", i);
        } else if (ch == 'm') { // jmethodID
          jmethodID mid = va_arg(ap, jmethodID);
          Method* m = reinterpret_cast<Method*>(mid);
          msg += PrettyMethod(m);
          if (!entry) {
            StringAppendF(&msg, " (%p)", mid);
          }
        } else if (ch == 'p') { // void* ("pointer")
          void* p = va_arg(ap, void*);
          if (p == NULL) {
            msg += "NULL";
          } else {
            StringAppendF(&msg, "(void*) %p", p);
          }
        } else if (ch == 'r') { // jint (release mode)
          jint releaseMode = va_arg(ap, jint);
          if (releaseMode == 0) {
            msg += "0";
          } else if (releaseMode == JNI_ABORT) {
            msg += "JNI_ABORT";
          } else if (releaseMode == JNI_COMMIT) {
            msg += "JNI_COMMIT";
          } else {
            StringAppendF(&msg, "invalid release mode %d", releaseMode);
          }
        } else if (ch == 'u') { // const char* (modified UTF-8)
          const char* utf = va_arg(ap, const char*);
          if (utf == NULL) {
            msg += "NULL";
          } else {
            StringAppendF(&msg, "\"%s\"", utf);
          }
        } else if (ch == '.') {
          msg += "...";
        } else {
          LOG(ERROR) << "unknown trace format specifier: " << ch;
          JniAbort();
          return;
        }
        if (*fmt) {
          StringAppendF(&msg, ", ");
        }
      }
      va_end(ap);

      if (entry) {
        if (mHasMethod) {
          std::string methodName(PrettyMethod(traceMethod, false));
          LOG(INFO) << "JNI: " << methodName << " -> " << mFunctionName << "(" << msg << ")";
          mIndent = methodName.size() + 1;
        } else {
          LOG(INFO) << "JNI: -> " << mFunctionName << "(" << msg << ")";
          mIndent = 0;
        }
      } else {
        LOG(INFO) << StringPrintf("JNI: %*s<- %s returned %s", mIndent, "", mFunctionName, msg.c_str());
      }
    }

    // We always do the thorough checks on entry, and never on exit...
    if (entry) {
      va_start(ap, fmt0);
      for (const char* fmt = fmt0; *fmt; ++fmt) {
        char ch = *fmt;
        if (ch == 'a') {
          checkArray(va_arg(ap, jarray));
        } else if (ch == 'c') {
          checkInstance(kClass, va_arg(ap, jclass));
        } else if (ch == 'L') {
          checkObject(va_arg(ap, jobject));
        } else if (ch == 'r') {
          checkReleaseMode(va_arg(ap, jint));
        } else if (ch == 's') {
          checkInstance(kString, va_arg(ap, jstring));
        } else if (ch == 'u') {
          if ((mFlags & kFlag_Release) != 0) {
            checkNonNull(va_arg(ap, const char*));
          } else {
            bool nullable = ((mFlags & kFlag_NullableUtf) != 0);
            checkUtfString(va_arg(ap, const char*), nullable);
          }
        } else if (ch == 'z') {
          checkLengthPositive(va_arg(ap, jsize));
        } else if (strchr("BCISZbfmpEv", ch) != NULL) {
          va_arg(ap, int); // Skip this argument.
        } else if (ch == 'D' || ch == 'F') {
          va_arg(ap, double); // Skip this argument.
        } else if (ch == 'J') {
          va_arg(ap, long); // Skip this argument.
        } else if (ch == '.') {
        } else {
          LOG(FATAL) << "unknown check format specifier: " << ch;
        }
      }
      va_end(ap);
    }
  }

private:
  void init(JNIEnv* env, JavaVM* vm, int flags, const char* functionName, bool hasMethod) {
    mEnv = reinterpret_cast<JNIEnvExt*>(env);
    mVm = reinterpret_cast<JavaVMExt*>(vm);
    mFlags = flags;
    mFunctionName = functionName;

    // Set "hasMethod" to true if we have a valid thread with a method pointer.
    // We won't have one before attaching a thread, after detaching a thread, or
    // after destroying the VM.
    mHasMethod = hasMethod;
  }

  /*
   * Verify that "array" is non-NULL and points to an Array object.
   *
   * Since we're dealing with objects, switch to "running" mode.
   */
  void checkArray(jarray java_array) {
    if (java_array == NULL) {
      LOG(ERROR) << "JNI ERROR: received null array";
      JniAbort();
      return;
    }

    ScopedJniThreadState ts(mEnv);
    Array* a = Decode<Array*>(ts, java_array);
    if (!Heap::IsHeapAddress(a)) {
      LOG(ERROR) << "JNI ERROR: jarray is an invalid " << GetIndirectRefKind(java_array) << ": " << reinterpret_cast<void*>(java_array);
      JniAbort();
    } else if (!a->IsArrayInstance()) {
      LOG(ERROR) << "JNI ERROR: jarray argument has non-array type: " << PrettyTypeOf(a);
      JniAbort();
    }
  }

  void checkLengthPositive(jsize length) {
    if (length < 0) {
      LOG(ERROR) << "JNI ERROR: negative jsize: " << length;
      JniAbort();
    }
  }

  /*
   * Verify that "jobj" is a valid object, and that it's an object that JNI
   * is allowed to know about.  We allow NULL references.
   *
   * Switches to "running" mode before performing checks.
   */
  void checkObject(jobject java_object) {
    if (java_object == NULL) {
      return;
    }

    ScopedJniThreadState ts(mEnv);

    Object* o = Decode<Object*>(ts, java_object);
    if (o != NULL && !Heap::IsHeapAddress(o)) {
      // TODO: when we remove work_around_app_jni_bugs, this should be impossible.
      LOG(ERROR) << "JNI ERROR: native code passing in reference to invalid " << GetIndirectRefKind(java_object) << ": " << java_object;
      JniAbort();
    }
  }

  /*
   * Verify that the "mode" argument passed to a primitive array Release
   * function is one of the valid values.
   */
  void checkReleaseMode(jint mode) {
    if (mode != 0 && mode != JNI_COMMIT && mode != JNI_ABORT) {
      LOG(ERROR) << "JNI ERROR: bad value for release mode: " << mode;
      JniAbort();
    }
  }

  void checkThread(int flags) {
    Thread* self = Thread::Current();
    if (self == NULL) {
      LOG(ERROR) << "JNI ERROR: non-VM thread making JNI calls";
      JniAbort();
      return;
    }

    // Get the *correct* JNIEnv by going through our TLS pointer.
    JNIEnvExt* threadEnv = self->GetJniEnv();

    /*
     * Verify that the current thread is (a) attached and (b) associated with
     * this particular instance of JNIEnv.
     */
    if (mEnv != threadEnv) {
      LOG(ERROR) << "JNI ERROR: thread " << *self << " using JNIEnv* from thread " << *mEnv->self;
      // If we're keeping broken code limping along, we need to suppress the abort...
      if (!mEnv->work_around_app_jni_bugs) {
        JniAbort();
        return;
      }
    }

    /*
     * Verify that, if this thread previously made a critical "get" call, we
     * do the corresponding "release" call before we try anything else.
     */
    switch (flags & kFlag_CritMask) {
    case kFlag_CritOkay:    // okay to call this method
      break;
    case kFlag_CritBad:     // not okay to call
      if (threadEnv->critical) {
        LOG(ERROR) << "JNI ERROR: thread " << *self << " using JNI after critical get";
        JniAbort();
        return;
      }
      break;
    case kFlag_CritGet:     // this is a "get" call
      /* don't check here; we allow nested gets */
      threadEnv->critical++;
      break;
    case kFlag_CritRelease: // this is a "release" call
      threadEnv->critical--;
      if (threadEnv->critical < 0) {
        LOG(ERROR) << "JNI ERROR: thread " << *self << " called too many critical releases";
        JniAbort();
        return;
      }
      break;
    default:
      LOG(FATAL) << "bad flags (internal error): " << flags;
    }

    /*
     * Verify that, if an exception has been raised, the native code doesn't
     * make any JNI calls other than the Exception* methods.
     */
    if ((flags & kFlag_ExcepOkay) == 0 && self->IsExceptionPending()) {
      LOG(ERROR) << "JNI ERROR: JNI method called with exception pending";
      LOG(ERROR) << "Pending exception is: TODO"; // TODO
      // TODO: dvmLogExceptionStackTrace();
      JniAbort();
      return;
    }
  }

  /*
   * Verify that "bytes" points to valid "modified UTF-8" data.
   */
  void checkUtfString(const char* bytes, bool nullable) {
    if (bytes == NULL) {
      if (!nullable) {
        LOG(ERROR) << "JNI ERROR: non-nullable const char* was NULL";
        JniAbort();
        return;
      }
      return;
    }

    const char* errorKind = NULL;
    uint8_t utf8 = checkUtfBytes(bytes, &errorKind);
    if (errorKind != NULL) {
      LOG(ERROR) << "JNI ERROR: input is not valid UTF-8: illegal " << errorKind << " byte " << StringPrintf("%#x", utf8);
      LOG(ERROR) << "           string: '" << bytes << "'";
      JniAbort();
      return;
    }
  }

  enum InstanceKind {
    kClass,
    kDirectByteBuffer,
    kString,
    kThrowable,
  };

  /*
   * Verify that "jobj" is a valid non-NULL object reference, and points to
   * an instance of expectedClass.
   *
   * Because we're looking at an object on the GC heap, we have to switch
   * to "running" mode before doing the checks.
   */
  void checkInstance(InstanceKind kind, jobject java_object) {
    const char* what = NULL;
    switch (kind) {
    case kClass:
      what = "jclass";
      break;
    case kDirectByteBuffer:
      what = "direct ByteBuffer";
      break;
    case kString:
      what = "jstring";
      break;
    case kThrowable:
      what = "jthrowable";
      break;
    default:
      CHECK(false) << static_cast<int>(kind);
    }

    if (java_object == NULL) {
      LOG(ERROR) << "JNI ERROR: received null " << what;
      JniAbort();
      return;
    }

    ScopedJniThreadState ts(mEnv);
    Object* obj = Decode<Object*>(ts, java_object);
    if (!Heap::IsHeapAddress(obj)) {
      LOG(ERROR) << "JNI ERROR: " << what << " is an invalid  " << GetIndirectRefKind(java_object) << ": " << java_object;
      JniAbort();
      return;
    }

    bool okay = true;
    switch (kind) {
    case kClass:
      okay = obj->IsClass();
      break;
    case kDirectByteBuffer:
      // TODO
      break;
    case kString:
      okay = obj->IsString();
      break;
    case kThrowable:
      // TODO
      break;
    }
    if (!okay) {
      LOG(ERROR) << "JNI ERROR: " << what << " has wrong type: " << PrettyTypeOf(obj);
      JniAbort();
    }
  }

  static uint8_t checkUtfBytes(const char* bytes, const char** errorKind) {
    while (*bytes != '\0') {
      uint8_t utf8 = *(bytes++);
      // Switch on the high four bits.
      switch (utf8 >> 4) {
      case 0x00:
      case 0x01:
      case 0x02:
      case 0x03:
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07:
        // Bit pattern 0xxx. No need for any extra bytes.
        break;
      case 0x08:
      case 0x09:
      case 0x0a:
      case 0x0b:
      case 0x0f:
        /*
         * Bit pattern 10xx or 1111, which are illegal start bytes.
         * Note: 1111 is valid for normal UTF-8, but not the
         * modified UTF-8 used here.
         */
        *errorKind = "start";
        return utf8;
      case 0x0e:
        // Bit pattern 1110, so there are two additional bytes.
        utf8 = *(bytes++);
        if ((utf8 & 0xc0) != 0x80) {
          *errorKind = "continuation";
          return utf8;
        }
        // Fall through to take care of the final byte.
      case 0x0c:
      case 0x0d:
        // Bit pattern 110x, so there is one additional byte.
        utf8 = *(bytes++);
        if ((utf8 & 0xc0) != 0x80) {
          *errorKind = "continuation";
          return utf8;
        }
        break;
      }
    }
    return 0;
  }

  void JniAbort() {
    ::art::JniAbort(mFunctionName);
  }

  JNIEnvExt* mEnv;
  JavaVMExt* mVm;
  const char* mFunctionName;
  int mFlags;
  bool mHasMethod;
  size_t mIndent;

  DISALLOW_COPY_AND_ASSIGN(ScopedCheck);
};

#define CHECK_JNI_ENTRY(flags, types, args...) \
  ScopedCheck sc(env, flags, __FUNCTION__); \
  sc.check(true, types, ##args)

#define CHECK_JNI_EXIT(type, exp) ({ \
  typeof (exp) _rc = (exp); \
  sc.check(false, type, _rc); \
  _rc; })
#define CHECK_JNI_EXIT_VOID() \
  sc.check(false, "V")

/*
 * ===========================================================================
 *      Guarded arrays
 * ===========================================================================
 */

#define kGuardLen       512         /* must be multiple of 2 */
#define kGuardPattern   0xd5e3      /* uncommon values; d5e3d5e3 invalid addr */
#define kGuardMagic     0xffd5aa96

/* this gets tucked in at the start of the buffer; struct size must be even */
struct GuardedCopy {
  uint32_t magic;
  uLong adler;
  size_t originalLen;
  const void* originalPtr;

  /* find the GuardedCopy given the pointer into the "live" data */
  static inline const GuardedCopy* fromData(const void* dataBuf) {
    return reinterpret_cast<const GuardedCopy*>(actualBuffer(dataBuf));
  }

  /*
   * Create an over-sized buffer to hold the contents of "buf".  Copy it in,
   * filling in the area around it with guard data.
   *
   * We use a 16-bit pattern to make a rogue memset less likely to elude us.
   */
  static void* create(const void* buf, size_t len, bool modOkay) {
    size_t newLen = actualLength(len);
    uint8_t* newBuf = debugAlloc(newLen);

    /* fill it in with a pattern */
    uint16_t* pat = (uint16_t*) newBuf;
    for (size_t i = 0; i < newLen / 2; i++) {
      *pat++ = kGuardPattern;
    }

    /* copy the data in; note "len" could be zero */
    memcpy(newBuf + kGuardLen / 2, buf, len);

    /* if modification is not expected, grab a checksum */
    uLong adler = 0;
    if (!modOkay) {
      adler = adler32(0L, Z_NULL, 0);
      adler = adler32(adler, (const Bytef*)buf, len);
      *(uLong*)newBuf = adler;
    }

    GuardedCopy* pExtra = reinterpret_cast<GuardedCopy*>(newBuf);
    pExtra->magic = kGuardMagic;
    pExtra->adler = adler;
    pExtra->originalPtr = buf;
    pExtra->originalLen = len;

    return newBuf + kGuardLen / 2;
  }

  /*
   * Free up the guard buffer, scrub it, and return the original pointer.
   */
  static void* destroy(void* dataBuf) {
    const GuardedCopy* pExtra = GuardedCopy::fromData(dataBuf);
    void* originalPtr = (void*) pExtra->originalPtr;
    size_t len = pExtra->originalLen;
    debugFree(dataBuf, len);
    return originalPtr;
  }

  /*
   * Verify the guard area and, if "modOkay" is false, that the data itself
   * has not been altered.
   *
   * The caller has already checked that "dataBuf" is non-NULL.
   */
  static void check(const char* functionName, const void* dataBuf, bool modOkay) {
    static const uint32_t kMagicCmp = kGuardMagic;
    const uint8_t* fullBuf = actualBuffer(dataBuf);
    const GuardedCopy* pExtra = GuardedCopy::fromData(dataBuf);

    /*
     * Before we do anything with "pExtra", check the magic number.  We
     * do the check with memcmp rather than "==" in case the pointer is
     * unaligned.  If it points to completely bogus memory we're going
     * to crash, but there's no easy way around that.
     */
    if (memcmp(&pExtra->magic, &kMagicCmp, 4) != 0) {
      uint8_t buf[4];
      memcpy(buf, &pExtra->magic, 4);
      LOG(ERROR) << StringPrintf("JNI: guard magic does not match "
          "(found 0x%02x%02x%02x%02x) -- incorrect data pointer %p?",
          buf[3], buf[2], buf[1], buf[0], dataBuf); /* assume little endian */
      JniAbort(functionName);
    }

    size_t len = pExtra->originalLen;

    /* check bottom half of guard; skip over optional checksum storage */
    const uint16_t* pat = (uint16_t*) fullBuf;
    for (size_t i = sizeof(GuardedCopy) / 2; i < (kGuardLen / 2 - sizeof(GuardedCopy)) / 2; i++) {
      if (pat[i] != kGuardPattern) {
        LOG(ERROR) << "JNI: guard pattern(1) disturbed at " << (void*) fullBuf << " + " << (i*2);
        JniAbort(functionName);
      }
    }

    int offset = kGuardLen / 2 + len;
    if (offset & 0x01) {
      /* odd byte; expected value depends on endian-ness of host */
      const uint16_t patSample = kGuardPattern;
      if (fullBuf[offset] != ((const uint8_t*) &patSample)[1]) {
        LOG(ERROR) << "JNI: guard pattern disturbed in odd byte after "
                   << (void*) fullBuf << " (+" << offset << ") "
                   << StringPrintf("0x%02x 0x%02x", fullBuf[offset], ((const uint8_t*) &patSample)[1]);
        JniAbort(functionName);
      }
      offset++;
    }

    /* check top half of guard */
    pat = (uint16_t*) (fullBuf + offset);
    for (size_t i = 0; i < kGuardLen / 4; i++) {
      if (pat[i] != kGuardPattern) {
        LOG(ERROR) << "JNI: guard pattern(2) disturbed at " << (void*) fullBuf << " + " << (offset + i*2);
        JniAbort(functionName);
      }
    }

    /*
     * If modification is not expected, verify checksum.  Strictly speaking
     * this is wrong: if we told the client that we made a copy, there's no
     * reason they can't alter the buffer.
     */
    if (!modOkay) {
      uLong adler = adler32(0L, Z_NULL, 0);
      adler = adler32(adler, (const Bytef*)dataBuf, len);
      if (pExtra->adler != adler) {
        LOG(ERROR) << StringPrintf("JNI: buffer modified (0x%08lx vs 0x%08lx) at addr %p", pExtra->adler, adler, dataBuf);
        JniAbort(functionName);
      }
    }
  }

 private:
  static uint8_t* debugAlloc(size_t len) {
    void* result = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    if (result == MAP_FAILED) {
      PLOG(FATAL) << "GuardedCopy::create mmap(" << len << ") failed";
    }
    return reinterpret_cast<uint8_t*>(result);
  }

  static void debugFree(void* dataBuf, size_t len) {
    uint8_t* fullBuf = actualBuffer(dataBuf);
    size_t totalByteCount = actualLength(len);
    // TODO: we could mprotect instead, and keep the allocation around for a while.
    // This would be even more expensive, but it might catch more errors.
    // if (mprotect(fullBuf, totalByteCount, PROT_NONE) != 0) {
    //     LOGW("mprotect(PROT_NONE) failed: %s", strerror(errno));
    // }
    if (munmap(fullBuf, totalByteCount) != 0) {
      PLOG(FATAL) << "munmap(" << (void*) fullBuf << ", " << totalByteCount << ") failed";
    }
  }

  static const uint8_t* actualBuffer(const void* dataBuf) {
    return reinterpret_cast<const uint8_t*>(dataBuf) - kGuardLen / 2;
  }

  static uint8_t* actualBuffer(void* dataBuf) {
    return reinterpret_cast<uint8_t*>(dataBuf) - kGuardLen / 2;
  }

  // Underlying length of a user allocation of 'length' bytes.
  static size_t actualLength(size_t length) {
    return (length + kGuardLen + 1) & ~0x01;
  }
};

/*
 * Create a guarded copy of a primitive array.  Modifications to the copied
 * data are allowed.  Returns a pointer to the copied data.
 */
void* CreateGuardedPACopy(JNIEnv* env, const jarray java_array, jboolean* isCopy) {
  ScopedJniThreadState ts(env);

  Array* a = Decode<Array*>(ts, java_array);
  size_t byte_count = a->GetLength() * a->GetClass()->GetComponentSize();
  void* result = GuardedCopy::create(a->GetRawData(), byte_count, true);
  if (isCopy != NULL) {
    *isCopy = JNI_TRUE;
  }
  return result;
}

/*
 * Perform the array "release" operation, which may or may not copy data
 * back into the VM, and may or may not release the underlying storage.
 */
void ReleaseGuardedPACopy(JNIEnv* env, jarray java_array, void* dataBuf, int mode) {
  if (reinterpret_cast<uintptr_t>(dataBuf) == kNoCopyMagic) {
    return;
  }

  ScopedJniThreadState ts(env);
  Array* a = Decode<Array*>(ts, java_array);

  GuardedCopy::check(__FUNCTION__, dataBuf, true);

  if (mode != JNI_ABORT) {
    size_t len = GuardedCopy::fromData(dataBuf)->originalLen;
    memcpy(a->GetRawData(), dataBuf, len);
  }
  if (mode != JNI_COMMIT) {
    GuardedCopy::destroy(dataBuf);
  }
}

/*
 * ===========================================================================
 *      JNI functions
 * ===========================================================================
 */

class CheckJNI {
 public:
  static jint GetVersion(JNIEnv* env) {
    CHECK_JNI_ENTRY(kFlag_Default, "E", env);
    return CHECK_JNI_EXIT("I", baseEnv(env)->GetVersion(env));
  }

  static jclass DefineClass(JNIEnv* env, const char* name, jobject loader, const jbyte* buf, jsize bufLen) {
    CHECK_JNI_ENTRY(kFlag_Default, "EuLpz", env, name, loader, buf, bufLen);
    sc.checkClassName(name);
    return CHECK_JNI_EXIT("c", baseEnv(env)->DefineClass(env, name, loader, buf, bufLen));
  }

  static jclass FindClass(JNIEnv* env, const char* name) {
    CHECK_JNI_ENTRY(kFlag_Default, "Eu", env, name);
    sc.checkClassName(name);
    return CHECK_JNI_EXIT("c", baseEnv(env)->FindClass(env, name));
  }

  static jclass GetSuperclass(JNIEnv* env, jclass clazz) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ec", env, clazz);
    return CHECK_JNI_EXIT("c", baseEnv(env)->GetSuperclass(env, clazz));
  }

  static jboolean IsAssignableFrom(JNIEnv* env, jclass clazz1, jclass clazz2) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ecc", env, clazz1, clazz2);
    return CHECK_JNI_EXIT("b", baseEnv(env)->IsAssignableFrom(env, clazz1, clazz2));
  }

  static jmethodID FromReflectedMethod(JNIEnv* env, jobject method) {
    CHECK_JNI_ENTRY(kFlag_Default, "EL", env, method);
    // TODO: check that 'field' is a java.lang.reflect.Method.
    return CHECK_JNI_EXIT("m", baseEnv(env)->FromReflectedMethod(env, method));
  }

  static jfieldID FromReflectedField(JNIEnv* env, jobject field) {
    CHECK_JNI_ENTRY(kFlag_Default, "EL", env, field);
    // TODO: check that 'field' is a java.lang.reflect.Field.
    return CHECK_JNI_EXIT("f", baseEnv(env)->FromReflectedField(env, field));
  }

  static jobject ToReflectedMethod(JNIEnv* env, jclass cls, jmethodID mid, jboolean isStatic) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ecmb", env, cls, mid, isStatic);
    return CHECK_JNI_EXIT("L", baseEnv(env)->ToReflectedMethod(env, cls, mid, isStatic));
  }

  static jobject ToReflectedField(JNIEnv* env, jclass cls, jfieldID fid, jboolean isStatic) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ecfb", env, cls, fid, isStatic);
    return CHECK_JNI_EXIT("L", baseEnv(env)->ToReflectedField(env, cls, fid, isStatic));
  }

  static jint Throw(JNIEnv* env, jthrowable obj) {
    CHECK_JNI_ENTRY(kFlag_Default, "EL", env, obj);
    // TODO: check that 'obj' is a java.lang.Throwable.
    return CHECK_JNI_EXIT("I", baseEnv(env)->Throw(env, obj));
  }

  static jint ThrowNew(JNIEnv* env, jclass clazz, const char* message) {
    CHECK_JNI_ENTRY(kFlag_NullableUtf, "Ecu", env, clazz, message);
    return CHECK_JNI_EXIT("I", baseEnv(env)->ThrowNew(env, clazz, message));
  }

  static jthrowable ExceptionOccurred(JNIEnv* env) {
    CHECK_JNI_ENTRY(kFlag_ExcepOkay, "E", env);
    return CHECK_JNI_EXIT("L", baseEnv(env)->ExceptionOccurred(env));
  }

  static void ExceptionDescribe(JNIEnv* env) {
    CHECK_JNI_ENTRY(kFlag_ExcepOkay, "E", env);
    baseEnv(env)->ExceptionDescribe(env);
    CHECK_JNI_EXIT_VOID();
  }

  static void ExceptionClear(JNIEnv* env) {
    CHECK_JNI_ENTRY(kFlag_ExcepOkay, "E", env);
    baseEnv(env)->ExceptionClear(env);
    CHECK_JNI_EXIT_VOID();
  }

  static void FatalError(JNIEnv* env, const char* msg) {
    CHECK_JNI_ENTRY(kFlag_NullableUtf, "Eu", env, msg);
    baseEnv(env)->FatalError(env, msg);
    CHECK_JNI_EXIT_VOID();
  }

  static jint PushLocalFrame(JNIEnv* env, jint capacity) {
    CHECK_JNI_ENTRY(kFlag_Default | kFlag_ExcepOkay, "EI", env, capacity);
    return CHECK_JNI_EXIT("I", baseEnv(env)->PushLocalFrame(env, capacity));
  }

  static jobject PopLocalFrame(JNIEnv* env, jobject res) {
    CHECK_JNI_ENTRY(kFlag_Default | kFlag_ExcepOkay, "EL", env, res);
    return CHECK_JNI_EXIT("L", baseEnv(env)->PopLocalFrame(env, res));
  }

  static jobject NewGlobalRef(JNIEnv* env, jobject obj) {
    CHECK_JNI_ENTRY(kFlag_Default, "EL", env, obj);
    return CHECK_JNI_EXIT("L", baseEnv(env)->NewGlobalRef(env, obj));
  }

  static jobject NewLocalRef(JNIEnv* env, jobject ref) {
    CHECK_JNI_ENTRY(kFlag_Default, "EL", env, ref);
    return CHECK_JNI_EXIT("L", baseEnv(env)->NewLocalRef(env, ref));
  }

  static void DeleteGlobalRef(JNIEnv* env, jobject globalRef) {
    CHECK_JNI_ENTRY(kFlag_Default | kFlag_ExcepOkay, "EL", env, globalRef);
    if (globalRef != NULL && GetIndirectRefKind(globalRef) != kGlobal) {
      LOG(ERROR) << "JNI ERROR: DeleteGlobalRef on " << GetIndirectRefKind(globalRef) << ": " << globalRef;
      JniAbort(__FUNCTION__);
    } else {
      baseEnv(env)->DeleteGlobalRef(env, globalRef);
      CHECK_JNI_EXIT_VOID();
    }
  }

  static void DeleteWeakGlobalRef(JNIEnv* env, jweak weakGlobalRef) {
    CHECK_JNI_ENTRY(kFlag_Default | kFlag_ExcepOkay, "EL", env, weakGlobalRef);
    if (weakGlobalRef != NULL && GetIndirectRefKind(weakGlobalRef) != kWeakGlobal) {
      LOG(ERROR) << "JNI ERROR: DeleteWeakGlobalRef on " << GetIndirectRefKind(weakGlobalRef) << ": " << weakGlobalRef;
      JniAbort(__FUNCTION__);
    } else {
      baseEnv(env)->DeleteWeakGlobalRef(env, weakGlobalRef);
      CHECK_JNI_EXIT_VOID();
    }
  }

  static void DeleteLocalRef(JNIEnv* env, jobject localRef) {
    CHECK_JNI_ENTRY(kFlag_Default | kFlag_ExcepOkay, "EL", env, localRef);
    if (localRef != NULL && GetIndirectRefKind(localRef) != kLocal) {
      LOG(ERROR) << "JNI ERROR: DeleteLocalRef on " << GetIndirectRefKind(localRef) << ": " << localRef;
      JniAbort(__FUNCTION__);
    } else {
      baseEnv(env)->DeleteLocalRef(env, localRef);
      CHECK_JNI_EXIT_VOID();
    }
  }

  static jint EnsureLocalCapacity(JNIEnv *env, jint capacity) {
    CHECK_JNI_ENTRY(kFlag_Default, "EI", env, capacity);
    return CHECK_JNI_EXIT("I", baseEnv(env)->EnsureLocalCapacity(env, capacity));
  }

  static jboolean IsSameObject(JNIEnv* env, jobject ref1, jobject ref2) {
    CHECK_JNI_ENTRY(kFlag_Default, "ELL", env, ref1, ref2);
    return CHECK_JNI_EXIT("b", baseEnv(env)->IsSameObject(env, ref1, ref2));
  }

  static jobject AllocObject(JNIEnv* env, jclass clazz) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ec", env, clazz);
    return CHECK_JNI_EXIT("L", baseEnv(env)->AllocObject(env, clazz));
  }

  static jobject NewObject(JNIEnv* env, jclass clazz, jmethodID mid, ...) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ecm.", env, clazz, mid);
    va_list args;
    va_start(args, mid);
    jobject result = baseEnv(env)->NewObjectV(env, clazz, mid, args);
    va_end(args);
    return CHECK_JNI_EXIT("L", result);
  }

  static jobject NewObjectV(JNIEnv* env, jclass clazz, jmethodID mid, va_list args) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ecm.", env, clazz, mid);
    return CHECK_JNI_EXIT("L", baseEnv(env)->NewObjectV(env, clazz, mid, args));
  }

  static jobject NewObjectA(JNIEnv* env, jclass clazz, jmethodID mid, jvalue* args) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ecm.", env, clazz, mid);
    return CHECK_JNI_EXIT("L", baseEnv(env)->NewObjectA(env, clazz, mid, args));
  }

  static jclass GetObjectClass(JNIEnv* env, jobject obj) {
    CHECK_JNI_ENTRY(kFlag_Default, "EL", env, obj);
    return CHECK_JNI_EXIT("c", baseEnv(env)->GetObjectClass(env, obj));
  }

  static jboolean IsInstanceOf(JNIEnv* env, jobject obj, jclass clazz) {
    CHECK_JNI_ENTRY(kFlag_Default, "ELc", env, obj, clazz);
    return CHECK_JNI_EXIT("b", baseEnv(env)->IsInstanceOf(env, obj, clazz));
  }

  static jmethodID GetMethodID(JNIEnv* env, jclass clazz, const char* name, const char* sig) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ecuu", env, clazz, name, sig);
    return CHECK_JNI_EXIT("m", baseEnv(env)->GetMethodID(env, clazz, name, sig));
  }

  static jfieldID GetFieldID(JNIEnv* env, jclass clazz, const char* name, const char* sig) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ecuu", env, clazz, name, sig);
    return CHECK_JNI_EXIT("f", baseEnv(env)->GetFieldID(env, clazz, name, sig));
  }

  static jmethodID GetStaticMethodID(JNIEnv* env, jclass clazz, const char* name, const char* sig) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ecuu", env, clazz, name, sig);
    return CHECK_JNI_EXIT("m", baseEnv(env)->GetStaticMethodID(env, clazz, name, sig));
  }

  static jfieldID GetStaticFieldID(JNIEnv* env, jclass clazz, const char* name, const char* sig) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ecuu", env, clazz, name, sig);
    return CHECK_JNI_EXIT("f", baseEnv(env)->GetStaticFieldID(env, clazz, name, sig));
  }

#define FIELD_ACCESSORS(_ctype, _jname, _type) \
    static _ctype GetStatic##_jname##Field(JNIEnv* env, jclass clazz, jfieldID fid) { \
        CHECK_JNI_ENTRY(kFlag_Default, "Ecf", env, clazz, fid); \
        sc.checkStaticFieldID(clazz, fid); \
        return CHECK_JNI_EXIT(_type, baseEnv(env)->GetStatic##_jname##Field(env, clazz, fid)); \
    } \
    static _ctype Get##_jname##Field(JNIEnv* env, jobject obj, jfieldID fid) { \
        CHECK_JNI_ENTRY(kFlag_Default, "ELf", env, obj, fid); \
        sc.checkInstanceFieldID(obj, fid); \
        return CHECK_JNI_EXIT(_type, baseEnv(env)->Get##_jname##Field(env, obj, fid)); \
    } \
    static void SetStatic##_jname##Field(JNIEnv* env, jclass clazz, jfieldID fid, _ctype value) { \
        CHECK_JNI_ENTRY(kFlag_Default, "Ecf" _type, env, clazz, fid, value); \
        sc.checkStaticFieldID(clazz, fid); \
        /* "value" arg only used when type == ref */ \
        sc.checkFieldType((jobject)(uint32_t)value, fid, _type[0], true); \
        baseEnv(env)->SetStatic##_jname##Field(env, clazz, fid, value); \
        CHECK_JNI_EXIT_VOID(); \
    } \
    static void Set##_jname##Field(JNIEnv* env, jobject obj, jfieldID fid, _ctype value) { \
        CHECK_JNI_ENTRY(kFlag_Default, "ELf" _type, env, obj, fid, value); \
        sc.checkInstanceFieldID(obj, fid); \
        /* "value" arg only used when type == ref */ \
        sc.checkFieldType((jobject)(uint32_t) value, fid, _type[0], false); \
        baseEnv(env)->Set##_jname##Field(env, obj, fid, value); \
        CHECK_JNI_EXIT_VOID(); \
    }

FIELD_ACCESSORS(jobject, Object, "L");
FIELD_ACCESSORS(jboolean, Boolean, "Z");
FIELD_ACCESSORS(jbyte, Byte, "B");
FIELD_ACCESSORS(jchar, Char, "C");
FIELD_ACCESSORS(jshort, Short, "S");
FIELD_ACCESSORS(jint, Int, "I");
FIELD_ACCESSORS(jlong, Long, "J");
FIELD_ACCESSORS(jfloat, Float, "F");
FIELD_ACCESSORS(jdouble, Double, "D");

#define CALL(_ctype, _jname, _retdecl, _retasgn, _retok, _retsig) \
    /* Virtual... */ \
    static _ctype Call##_jname##Method(JNIEnv* env, jobject obj, \
        jmethodID mid, ...) \
    { \
        CHECK_JNI_ENTRY(kFlag_Default, "ELm.", env, obj, mid); /* TODO: args! */ \
        sc.checkSig(mid, _retsig, false); \
        sc.checkVirtualMethod(obj, mid); \
        _retdecl; \
        va_list args; \
        va_start(args, mid); \
        _retasgn baseEnv(env)->Call##_jname##MethodV(env, obj, mid, args); \
        va_end(args); \
        _retok; \
    } \
    static _ctype Call##_jname##MethodV(JNIEnv* env, jobject obj, \
        jmethodID mid, va_list args) \
    { \
        CHECK_JNI_ENTRY(kFlag_Default, "ELm.", env, obj, mid); /* TODO: args! */ \
        sc.checkSig(mid, _retsig, false); \
        sc.checkVirtualMethod(obj, mid); \
        _retdecl; \
        _retasgn baseEnv(env)->Call##_jname##MethodV(env, obj, mid, args); \
        _retok; \
    } \
    static _ctype Call##_jname##MethodA(JNIEnv* env, jobject obj, \
        jmethodID mid, jvalue* args) \
    { \
        CHECK_JNI_ENTRY(kFlag_Default, "ELm.", env, obj, mid); /* TODO: args! */ \
        sc.checkSig(mid, _retsig, false); \
        sc.checkVirtualMethod(obj, mid); \
        _retdecl; \
        _retasgn baseEnv(env)->Call##_jname##MethodA(env, obj, mid, args); \
        _retok; \
    } \
    /* Non-virtual... */ \
    static _ctype CallNonvirtual##_jname##Method(JNIEnv* env, \
        jobject obj, jclass clazz, jmethodID mid, ...) \
    { \
        CHECK_JNI_ENTRY(kFlag_Default, "ELcm.", env, obj, clazz, mid); /* TODO: args! */ \
        sc.checkSig(mid, _retsig, false); \
        sc.checkVirtualMethod(obj, mid); \
        _retdecl; \
        va_list args; \
        va_start(args, mid); \
        _retasgn baseEnv(env)->CallNonvirtual##_jname##MethodV(env, obj, clazz, mid, args); \
        va_end(args); \
        _retok; \
    } \
    static _ctype CallNonvirtual##_jname##MethodV(JNIEnv* env, \
        jobject obj, jclass clazz, jmethodID mid, va_list args) \
    { \
        CHECK_JNI_ENTRY(kFlag_Default, "ELcm.", env, obj, clazz, mid); /* TODO: args! */ \
        sc.checkSig(mid, _retsig, false); \
        sc.checkVirtualMethod(obj, mid); \
        _retdecl; \
        _retasgn baseEnv(env)->CallNonvirtual##_jname##MethodV(env, obj, clazz, mid, args); \
        _retok; \
    } \
    static _ctype CallNonvirtual##_jname##MethodA(JNIEnv* env, \
        jobject obj, jclass clazz, jmethodID mid, jvalue* args) \
    { \
        CHECK_JNI_ENTRY(kFlag_Default, "ELcm.", env, obj, clazz, mid); /* TODO: args! */ \
        sc.checkSig(mid, _retsig, false); \
        sc.checkVirtualMethod(obj, mid); \
        _retdecl; \
        _retasgn baseEnv(env)->CallNonvirtual##_jname##MethodA(env, obj, clazz, mid, args); \
        _retok; \
    } \
    /* Static... */ \
    static _ctype CallStatic##_jname##Method(JNIEnv* env, \
        jclass clazz, jmethodID mid, ...) \
    { \
        CHECK_JNI_ENTRY(kFlag_Default, "Ecm.", env, clazz, mid); /* TODO: args! */ \
        sc.checkSig(mid, _retsig, true); \
        sc.checkStaticMethod(clazz, mid); \
        _retdecl; \
        va_list args; \
        va_start(args, mid); \
        _retasgn baseEnv(env)->CallStatic##_jname##MethodV(env, clazz, mid, args); \
        va_end(args); \
        _retok; \
    } \
    static _ctype CallStatic##_jname##MethodV(JNIEnv* env, \
        jclass clazz, jmethodID mid, va_list args) \
    { \
        CHECK_JNI_ENTRY(kFlag_Default, "Ecm.", env, clazz, mid); /* TODO: args! */ \
        sc.checkSig(mid, _retsig, true); \
        sc.checkStaticMethod(clazz, mid); \
        _retdecl; \
        _retasgn baseEnv(env)->CallStatic##_jname##MethodV(env, clazz, mid, args); \
        _retok; \
    } \
    static _ctype CallStatic##_jname##MethodA(JNIEnv* env, \
        jclass clazz, jmethodID mid, jvalue* args) \
    { \
        CHECK_JNI_ENTRY(kFlag_Default, "Ecm.", env, clazz, mid); /* TODO: args! */ \
        sc.checkSig(mid, _retsig, true); \
        sc.checkStaticMethod(clazz, mid); \
        _retdecl; \
        _retasgn baseEnv(env)->CallStatic##_jname##MethodA(env, clazz, mid, args); \
        _retok; \
    }

#define NON_VOID_RETURN(_retsig, _ctype) return CHECK_JNI_EXIT(_retsig, (_ctype) result)
#define VOID_RETURN CHECK_JNI_EXIT_VOID()

CALL(jobject, Object, Object* result, result=(Object*), NON_VOID_RETURN("L", jobject), "L");
CALL(jboolean, Boolean, jboolean result, result=, NON_VOID_RETURN("Z", jboolean), "Z");
CALL(jbyte, Byte, jbyte result, result=, NON_VOID_RETURN("B", jbyte), "B");
CALL(jchar, Char, jchar result, result=, NON_VOID_RETURN("C", jchar), "C");
CALL(jshort, Short, jshort result, result=, NON_VOID_RETURN("S", jshort), "S");
CALL(jint, Int, jint result, result=, NON_VOID_RETURN("I", jint), "I");
CALL(jlong, Long, jlong result, result=, NON_VOID_RETURN("J", jlong), "J");
CALL(jfloat, Float, jfloat result, result=, NON_VOID_RETURN("F", jfloat), "F");
CALL(jdouble, Double, jdouble result, result=, NON_VOID_RETURN("D", jdouble), "D");
CALL(void, Void, , , VOID_RETURN, "V");

  static jstring NewString(JNIEnv* env, const jchar* unicodeChars, jsize len) {
    CHECK_JNI_ENTRY(kFlag_Default, "Epz", env, unicodeChars, len);
    return CHECK_JNI_EXIT("s", baseEnv(env)->NewString(env, unicodeChars, len));
  }

  static jsize GetStringLength(JNIEnv* env, jstring string) {
    CHECK_JNI_ENTRY(kFlag_CritOkay, "Es", env, string);
    return CHECK_JNI_EXIT("I", baseEnv(env)->GetStringLength(env, string));
  }

  static const jchar* GetStringChars(JNIEnv* env, jstring java_string, jboolean* isCopy) {
    CHECK_JNI_ENTRY(kFlag_CritOkay, "Esp", env, java_string, isCopy);
    const jchar* result = baseEnv(env)->GetStringChars(env, java_string, isCopy);
    if (sc.forceCopy() && result != NULL) {
      ScopedJniThreadState ts(env);
      String* s = Decode<String*>(ts, java_string);
      int byteCount = s->GetLength() * 2;
      result = (const jchar*) GuardedCopy::create(result, byteCount, false);
      if (isCopy != NULL) {
        *isCopy = JNI_TRUE;
      }
    }
    return CHECK_JNI_EXIT("p", result);
  }

  static void ReleaseStringChars(JNIEnv* env, jstring string, const jchar* chars) {
    CHECK_JNI_ENTRY(kFlag_Default | kFlag_ExcepOkay, "Esp", env, string, chars);
    sc.checkNonNull(chars);
    if (sc.forceCopy()) {
      GuardedCopy::check(__FUNCTION__, chars, false);
      chars = (const jchar*) GuardedCopy::destroy((jchar*)chars);
    }
    baseEnv(env)->ReleaseStringChars(env, string, chars);
    CHECK_JNI_EXIT_VOID();
  }

  static jstring NewStringUTF(JNIEnv* env, const char* bytes) {
    CHECK_JNI_ENTRY(kFlag_NullableUtf, "Eu", env, bytes); // TODO: show pointer and truncate string.
    return CHECK_JNI_EXIT("s", baseEnv(env)->NewStringUTF(env, bytes));
  }

  static jsize GetStringUTFLength(JNIEnv* env, jstring string) {
    CHECK_JNI_ENTRY(kFlag_CritOkay, "Es", env, string);
    return CHECK_JNI_EXIT("I", baseEnv(env)->GetStringUTFLength(env, string));
  }

  static const char* GetStringUTFChars(JNIEnv* env, jstring string, jboolean* isCopy) {
    CHECK_JNI_ENTRY(kFlag_CritOkay, "Esp", env, string, isCopy);
    const char* result = baseEnv(env)->GetStringUTFChars(env, string, isCopy);
    if (sc.forceCopy() && result != NULL) {
      result = (const char*) GuardedCopy::create(result, strlen(result) + 1, false);
      if (isCopy != NULL) {
        *isCopy = JNI_TRUE;
      }
    }
    return CHECK_JNI_EXIT("u", result); // TODO: show pointer and truncate string.
  }

  static void ReleaseStringUTFChars(JNIEnv* env, jstring string, const char* utf) {
    CHECK_JNI_ENTRY(kFlag_ExcepOkay | kFlag_Release, "Esu", env, string, utf); // TODO: show pointer and truncate string.
    if (sc.forceCopy()) {
      GuardedCopy::check(__FUNCTION__, utf, false);
      utf = (const char*) GuardedCopy::destroy((char*)utf);
    }
    baseEnv(env)->ReleaseStringUTFChars(env, string, utf);
    CHECK_JNI_EXIT_VOID();
  }

  static jsize GetArrayLength(JNIEnv* env, jarray array) {
    CHECK_JNI_ENTRY(kFlag_CritOkay, "Ea", env, array);
    return CHECK_JNI_EXIT("I", baseEnv(env)->GetArrayLength(env, array));
  }

  static jobjectArray NewObjectArray(JNIEnv* env, jsize length, jclass elementClass, jobject initialElement) {
    CHECK_JNI_ENTRY(kFlag_Default, "EzcL", env, length, elementClass, initialElement);
    return CHECK_JNI_EXIT("a", baseEnv(env)->NewObjectArray(env, length, elementClass, initialElement));
  }

  static jobject GetObjectArrayElement(JNIEnv* env, jobjectArray array, jsize index) {
    CHECK_JNI_ENTRY(kFlag_Default, "EaI", env, array, index);
    return CHECK_JNI_EXIT("L", baseEnv(env)->GetObjectArrayElement(env, array, index));
  }

  static void SetObjectArrayElement(JNIEnv* env, jobjectArray array, jsize index, jobject value) {
    CHECK_JNI_ENTRY(kFlag_Default, "EaIL", env, array, index, value);
    baseEnv(env)->SetObjectArrayElement(env, array, index, value);
    CHECK_JNI_EXIT_VOID();
  }

#define NEW_PRIMITIVE_ARRAY(_artype, _jname) \
    static _artype New##_jname##Array(JNIEnv* env, jsize length) { \
        CHECK_JNI_ENTRY(kFlag_Default, "Ez", env, length); \
        return CHECK_JNI_EXIT("a", baseEnv(env)->New##_jname##Array(env, length)); \
    }
NEW_PRIMITIVE_ARRAY(jbooleanArray, Boolean);
NEW_PRIMITIVE_ARRAY(jbyteArray, Byte);
NEW_PRIMITIVE_ARRAY(jcharArray, Char);
NEW_PRIMITIVE_ARRAY(jshortArray, Short);
NEW_PRIMITIVE_ARRAY(jintArray, Int);
NEW_PRIMITIVE_ARRAY(jlongArray, Long);
NEW_PRIMITIVE_ARRAY(jfloatArray, Float);
NEW_PRIMITIVE_ARRAY(jdoubleArray, Double);

class ForceCopyGetChecker {
public:
  ForceCopyGetChecker(ScopedCheck& sc, jboolean* isCopy) {
    forceCopy = sc.forceCopy();
    noCopy = 0;
    if (forceCopy && isCopy != NULL) {
      /* capture this before the base call tramples on it */
      noCopy = *(uint32_t*) isCopy;
    }
  }

  template<typename ResultT>
  ResultT check(JNIEnv* env, jarray array, jboolean* isCopy, ResultT result) {
    if (forceCopy && result != NULL) {
      if (noCopy != kNoCopyMagic) {
        result = reinterpret_cast<ResultT>(CreateGuardedPACopy(env, array, isCopy));
      }
    }
    return result;
  }

  uint32_t noCopy;
  bool forceCopy;
};

#define GET_PRIMITIVE_ARRAY_ELEMENTS(_ctype, _jname) \
  static _ctype* Get##_jname##ArrayElements(JNIEnv* env, _ctype##Array array, jboolean* isCopy) { \
    CHECK_JNI_ENTRY(kFlag_Default, "Eap", env, array, isCopy); \
    _ctype* result = ForceCopyGetChecker(sc, isCopy).check(env, array, isCopy, baseEnv(env)->Get##_jname##ArrayElements(env, array, isCopy)); \
    return CHECK_JNI_EXIT("p", result); \
  }

#define RELEASE_PRIMITIVE_ARRAY_ELEMENTS(_ctype, _jname) \
  static void Release##_jname##ArrayElements(JNIEnv* env, _ctype##Array array, _ctype* elems, jint mode) { \
    CHECK_JNI_ENTRY(kFlag_Default | kFlag_ExcepOkay, "Eapr", env, array, elems, mode); \
    sc.checkNonNull(elems); \
    if (sc.forceCopy()) { \
      ReleaseGuardedPACopy(env, array, elems, mode); \
    } \
    baseEnv(env)->Release##_jname##ArrayElements(env, array, elems, mode); \
    CHECK_JNI_EXIT_VOID(); \
  }

#define GET_PRIMITIVE_ARRAY_REGION(_ctype, _jname) \
    static void Get##_jname##ArrayRegion(JNIEnv* env, _ctype##Array array, jsize start, jsize len, _ctype* buf) { \
        CHECK_JNI_ENTRY(kFlag_Default, "EaIIp", env, array, start, len, buf); \
        baseEnv(env)->Get##_jname##ArrayRegion(env, array, start, len, buf); \
        CHECK_JNI_EXIT_VOID(); \
    }

#define SET_PRIMITIVE_ARRAY_REGION(_ctype, _jname) \
    static void Set##_jname##ArrayRegion(JNIEnv* env, _ctype##Array array, jsize start, jsize len, const _ctype* buf) { \
        CHECK_JNI_ENTRY(kFlag_Default, "EaIIp", env, array, start, len, buf); \
        baseEnv(env)->Set##_jname##ArrayRegion(env, array, start, len, buf); \
        CHECK_JNI_EXIT_VOID(); \
    }

#define PRIMITIVE_ARRAY_FUNCTIONS(_ctype, _jname, _typechar) \
    GET_PRIMITIVE_ARRAY_ELEMENTS(_ctype, _jname); \
    RELEASE_PRIMITIVE_ARRAY_ELEMENTS(_ctype, _jname); \
    GET_PRIMITIVE_ARRAY_REGION(_ctype, _jname); \
    SET_PRIMITIVE_ARRAY_REGION(_ctype, _jname);

/* TODO: verify primitive array type matches call type */
PRIMITIVE_ARRAY_FUNCTIONS(jboolean, Boolean, 'Z');
PRIMITIVE_ARRAY_FUNCTIONS(jbyte, Byte, 'B');
PRIMITIVE_ARRAY_FUNCTIONS(jchar, Char, 'C');
PRIMITIVE_ARRAY_FUNCTIONS(jshort, Short, 'S');
PRIMITIVE_ARRAY_FUNCTIONS(jint, Int, 'I');
PRIMITIVE_ARRAY_FUNCTIONS(jlong, Long, 'J');
PRIMITIVE_ARRAY_FUNCTIONS(jfloat, Float, 'F');
PRIMITIVE_ARRAY_FUNCTIONS(jdouble, Double, 'D');

  static jint RegisterNatives(JNIEnv* env, jclass clazz, const JNINativeMethod* methods, jint nMethods) {
    CHECK_JNI_ENTRY(kFlag_Default, "EcpI", env, clazz, methods, nMethods);
    return CHECK_JNI_EXIT("I", baseEnv(env)->RegisterNatives(env, clazz, methods, nMethods));
  }

  static jint UnregisterNatives(JNIEnv* env, jclass clazz) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ec", env, clazz);
    return CHECK_JNI_EXIT("I", baseEnv(env)->UnregisterNatives(env, clazz));
  }

  static jint MonitorEnter(JNIEnv* env, jobject obj) {
    CHECK_JNI_ENTRY(kFlag_Default, "EL", env, obj);
    return CHECK_JNI_EXIT("I", baseEnv(env)->MonitorEnter(env, obj));
  }

  static jint MonitorExit(JNIEnv* env, jobject obj) {
    CHECK_JNI_ENTRY(kFlag_Default | kFlag_ExcepOkay, "EL", env, obj);
    return CHECK_JNI_EXIT("I", baseEnv(env)->MonitorExit(env, obj));
  }

  static jint GetJavaVM(JNIEnv *env, JavaVM **vm) {
    CHECK_JNI_ENTRY(kFlag_Default, "Ep", env, vm);
    return CHECK_JNI_EXIT("I", baseEnv(env)->GetJavaVM(env, vm));
  }

  static void GetStringRegion(JNIEnv* env, jstring str, jsize start, jsize len, jchar* buf) {
    CHECK_JNI_ENTRY(kFlag_CritOkay, "EsIIp", env, str, start, len, buf);
    baseEnv(env)->GetStringRegion(env, str, start, len, buf);
    CHECK_JNI_EXIT_VOID();
  }

  static void GetStringUTFRegion(JNIEnv* env, jstring str, jsize start, jsize len, char* buf) {
    CHECK_JNI_ENTRY(kFlag_CritOkay, "EsIIp", env, str, start, len, buf);
    baseEnv(env)->GetStringUTFRegion(env, str, start, len, buf);
    CHECK_JNI_EXIT_VOID();
  }

  static void* GetPrimitiveArrayCritical(JNIEnv* env, jarray array, jboolean* isCopy) {
    CHECK_JNI_ENTRY(kFlag_CritGet, "Eap", env, array, isCopy);
    void* result = baseEnv(env)->GetPrimitiveArrayCritical(env, array, isCopy);
    if (sc.forceCopy() && result != NULL) {
      result = CreateGuardedPACopy(env, array, isCopy);
    }
    return CHECK_JNI_EXIT("p", result);
  }

  static void ReleasePrimitiveArrayCritical(JNIEnv* env, jarray array, void* carray, jint mode) {
    CHECK_JNI_ENTRY(kFlag_CritRelease | kFlag_ExcepOkay, "Eapr", env, array, carray, mode);
    sc.checkNonNull(carray);
    if (sc.forceCopy()) {
      ReleaseGuardedPACopy(env, array, carray, mode);
    }
    baseEnv(env)->ReleasePrimitiveArrayCritical(env, array, carray, mode);
    CHECK_JNI_EXIT_VOID();
  }

  static const jchar* GetStringCritical(JNIEnv* env, jstring java_string, jboolean* isCopy) {
    CHECK_JNI_ENTRY(kFlag_CritGet, "Esp", env, java_string, isCopy);
    const jchar* result = baseEnv(env)->GetStringCritical(env, java_string, isCopy);
    if (sc.forceCopy() && result != NULL) {
      ScopedJniThreadState ts(env);
      String* s = Decode<String*>(ts, java_string);
      int byteCount = s->GetLength() * 2;
      result = (const jchar*) GuardedCopy::create(result, byteCount, false);
      if (isCopy != NULL) {
        *isCopy = JNI_TRUE;
      }
    }
    return CHECK_JNI_EXIT("p", result);
  }

  static void ReleaseStringCritical(JNIEnv* env, jstring string, const jchar* carray) {
    CHECK_JNI_ENTRY(kFlag_CritRelease | kFlag_ExcepOkay, "Esp", env, string, carray);
    sc.checkNonNull(carray);
    if (sc.forceCopy()) {
      GuardedCopy::check(__FUNCTION__, carray, false);
      carray = (const jchar*) GuardedCopy::destroy((jchar*)carray);
    }
    baseEnv(env)->ReleaseStringCritical(env, string, carray);
    CHECK_JNI_EXIT_VOID();
  }

  static jweak NewWeakGlobalRef(JNIEnv* env, jobject obj) {
    CHECK_JNI_ENTRY(kFlag_Default, "EL", env, obj);
    return CHECK_JNI_EXIT("L", baseEnv(env)->NewWeakGlobalRef(env, obj));
  }

  static jboolean ExceptionCheck(JNIEnv* env) {
    CHECK_JNI_ENTRY(kFlag_CritOkay | kFlag_ExcepOkay, "E", env);
    return CHECK_JNI_EXIT("b", baseEnv(env)->ExceptionCheck(env));
  }

  static jobjectRefType GetObjectRefType(JNIEnv* env, jobject obj) {
    // Note: we use "Ep" rather than "EL" because this is the one JNI function
    // that it's okay to pass an invalid reference to.
    CHECK_JNI_ENTRY(kFlag_Default, "Ep", env, obj);
    // TODO: proper decoding of jobjectRefType!
    return CHECK_JNI_EXIT("I", baseEnv(env)->GetObjectRefType(env, obj));
  }

  static jobject NewDirectByteBuffer(JNIEnv* env, void* address, jlong capacity) {
    CHECK_JNI_ENTRY(kFlag_Default, "EpJ", env, address, capacity);
    if (address == NULL) {
      LOG(ERROR) << "JNI ERROR: non-nullable address is NULL";
      JniAbort(__FUNCTION__);
    }
    if (capacity <= 0) {
      LOG(ERROR) << "JNI ERROR: capacity must be greater than 0: " << capacity;
      JniAbort(__FUNCTION__);
    }
    return CHECK_JNI_EXIT("L", baseEnv(env)->NewDirectByteBuffer(env, address, capacity));
  }

  static void* GetDirectBufferAddress(JNIEnv* env, jobject buf) {
    CHECK_JNI_ENTRY(kFlag_Default, "EL", env, buf);
    // TODO: check that 'buf' is a java.nio.Buffer.
    return CHECK_JNI_EXIT("p", baseEnv(env)->GetDirectBufferAddress(env, buf));
  }

  static jlong GetDirectBufferCapacity(JNIEnv* env, jobject buf) {
    CHECK_JNI_ENTRY(kFlag_Default, "EL", env, buf);
    // TODO: check that 'buf' is a java.nio.Buffer.
    return CHECK_JNI_EXIT("J", baseEnv(env)->GetDirectBufferCapacity(env, buf));
  }

 private:
  static inline const JNINativeInterface* baseEnv(JNIEnv* env) {
    return reinterpret_cast<JNIEnvExt*>(env)->unchecked_functions;
  }
};

const JNINativeInterface gCheckNativeInterface = {
  NULL,  // reserved0.
  NULL,  // reserved1.
  NULL,  // reserved2.
  NULL,  // reserved3.
  CheckJNI::GetVersion,
  CheckJNI::DefineClass,
  CheckJNI::FindClass,
  CheckJNI::FromReflectedMethod,
  CheckJNI::FromReflectedField,
  CheckJNI::ToReflectedMethod,
  CheckJNI::GetSuperclass,
  CheckJNI::IsAssignableFrom,
  CheckJNI::ToReflectedField,
  CheckJNI::Throw,
  CheckJNI::ThrowNew,
  CheckJNI::ExceptionOccurred,
  CheckJNI::ExceptionDescribe,
  CheckJNI::ExceptionClear,
  CheckJNI::FatalError,
  CheckJNI::PushLocalFrame,
  CheckJNI::PopLocalFrame,
  CheckJNI::NewGlobalRef,
  CheckJNI::DeleteGlobalRef,
  CheckJNI::DeleteLocalRef,
  CheckJNI::IsSameObject,
  CheckJNI::NewLocalRef,
  CheckJNI::EnsureLocalCapacity,
  CheckJNI::AllocObject,
  CheckJNI::NewObject,
  CheckJNI::NewObjectV,
  CheckJNI::NewObjectA,
  CheckJNI::GetObjectClass,
  CheckJNI::IsInstanceOf,
  CheckJNI::GetMethodID,
  CheckJNI::CallObjectMethod,
  CheckJNI::CallObjectMethodV,
  CheckJNI::CallObjectMethodA,
  CheckJNI::CallBooleanMethod,
  CheckJNI::CallBooleanMethodV,
  CheckJNI::CallBooleanMethodA,
  CheckJNI::CallByteMethod,
  CheckJNI::CallByteMethodV,
  CheckJNI::CallByteMethodA,
  CheckJNI::CallCharMethod,
  CheckJNI::CallCharMethodV,
  CheckJNI::CallCharMethodA,
  CheckJNI::CallShortMethod,
  CheckJNI::CallShortMethodV,
  CheckJNI::CallShortMethodA,
  CheckJNI::CallIntMethod,
  CheckJNI::CallIntMethodV,
  CheckJNI::CallIntMethodA,
  CheckJNI::CallLongMethod,
  CheckJNI::CallLongMethodV,
  CheckJNI::CallLongMethodA,
  CheckJNI::CallFloatMethod,
  CheckJNI::CallFloatMethodV,
  CheckJNI::CallFloatMethodA,
  CheckJNI::CallDoubleMethod,
  CheckJNI::CallDoubleMethodV,
  CheckJNI::CallDoubleMethodA,
  CheckJNI::CallVoidMethod,
  CheckJNI::CallVoidMethodV,
  CheckJNI::CallVoidMethodA,
  CheckJNI::CallNonvirtualObjectMethod,
  CheckJNI::CallNonvirtualObjectMethodV,
  CheckJNI::CallNonvirtualObjectMethodA,
  CheckJNI::CallNonvirtualBooleanMethod,
  CheckJNI::CallNonvirtualBooleanMethodV,
  CheckJNI::CallNonvirtualBooleanMethodA,
  CheckJNI::CallNonvirtualByteMethod,
  CheckJNI::CallNonvirtualByteMethodV,
  CheckJNI::CallNonvirtualByteMethodA,
  CheckJNI::CallNonvirtualCharMethod,
  CheckJNI::CallNonvirtualCharMethodV,
  CheckJNI::CallNonvirtualCharMethodA,
  CheckJNI::CallNonvirtualShortMethod,
  CheckJNI::CallNonvirtualShortMethodV,
  CheckJNI::CallNonvirtualShortMethodA,
  CheckJNI::CallNonvirtualIntMethod,
  CheckJNI::CallNonvirtualIntMethodV,
  CheckJNI::CallNonvirtualIntMethodA,
  CheckJNI::CallNonvirtualLongMethod,
  CheckJNI::CallNonvirtualLongMethodV,
  CheckJNI::CallNonvirtualLongMethodA,
  CheckJNI::CallNonvirtualFloatMethod,
  CheckJNI::CallNonvirtualFloatMethodV,
  CheckJNI::CallNonvirtualFloatMethodA,
  CheckJNI::CallNonvirtualDoubleMethod,
  CheckJNI::CallNonvirtualDoubleMethodV,
  CheckJNI::CallNonvirtualDoubleMethodA,
  CheckJNI::CallNonvirtualVoidMethod,
  CheckJNI::CallNonvirtualVoidMethodV,
  CheckJNI::CallNonvirtualVoidMethodA,
  CheckJNI::GetFieldID,
  CheckJNI::GetObjectField,
  CheckJNI::GetBooleanField,
  CheckJNI::GetByteField,
  CheckJNI::GetCharField,
  CheckJNI::GetShortField,
  CheckJNI::GetIntField,
  CheckJNI::GetLongField,
  CheckJNI::GetFloatField,
  CheckJNI::GetDoubleField,
  CheckJNI::SetObjectField,
  CheckJNI::SetBooleanField,
  CheckJNI::SetByteField,
  CheckJNI::SetCharField,
  CheckJNI::SetShortField,
  CheckJNI::SetIntField,
  CheckJNI::SetLongField,
  CheckJNI::SetFloatField,
  CheckJNI::SetDoubleField,
  CheckJNI::GetStaticMethodID,
  CheckJNI::CallStaticObjectMethod,
  CheckJNI::CallStaticObjectMethodV,
  CheckJNI::CallStaticObjectMethodA,
  CheckJNI::CallStaticBooleanMethod,
  CheckJNI::CallStaticBooleanMethodV,
  CheckJNI::CallStaticBooleanMethodA,
  CheckJNI::CallStaticByteMethod,
  CheckJNI::CallStaticByteMethodV,
  CheckJNI::CallStaticByteMethodA,
  CheckJNI::CallStaticCharMethod,
  CheckJNI::CallStaticCharMethodV,
  CheckJNI::CallStaticCharMethodA,
  CheckJNI::CallStaticShortMethod,
  CheckJNI::CallStaticShortMethodV,
  CheckJNI::CallStaticShortMethodA,
  CheckJNI::CallStaticIntMethod,
  CheckJNI::CallStaticIntMethodV,
  CheckJNI::CallStaticIntMethodA,
  CheckJNI::CallStaticLongMethod,
  CheckJNI::CallStaticLongMethodV,
  CheckJNI::CallStaticLongMethodA,
  CheckJNI::CallStaticFloatMethod,
  CheckJNI::CallStaticFloatMethodV,
  CheckJNI::CallStaticFloatMethodA,
  CheckJNI::CallStaticDoubleMethod,
  CheckJNI::CallStaticDoubleMethodV,
  CheckJNI::CallStaticDoubleMethodA,
  CheckJNI::CallStaticVoidMethod,
  CheckJNI::CallStaticVoidMethodV,
  CheckJNI::CallStaticVoidMethodA,
  CheckJNI::GetStaticFieldID,
  CheckJNI::GetStaticObjectField,
  CheckJNI::GetStaticBooleanField,
  CheckJNI::GetStaticByteField,
  CheckJNI::GetStaticCharField,
  CheckJNI::GetStaticShortField,
  CheckJNI::GetStaticIntField,
  CheckJNI::GetStaticLongField,
  CheckJNI::GetStaticFloatField,
  CheckJNI::GetStaticDoubleField,
  CheckJNI::SetStaticObjectField,
  CheckJNI::SetStaticBooleanField,
  CheckJNI::SetStaticByteField,
  CheckJNI::SetStaticCharField,
  CheckJNI::SetStaticShortField,
  CheckJNI::SetStaticIntField,
  CheckJNI::SetStaticLongField,
  CheckJNI::SetStaticFloatField,
  CheckJNI::SetStaticDoubleField,
  CheckJNI::NewString,
  CheckJNI::GetStringLength,
  CheckJNI::GetStringChars,
  CheckJNI::ReleaseStringChars,
  CheckJNI::NewStringUTF,
  CheckJNI::GetStringUTFLength,
  CheckJNI::GetStringUTFChars,
  CheckJNI::ReleaseStringUTFChars,
  CheckJNI::GetArrayLength,
  CheckJNI::NewObjectArray,
  CheckJNI::GetObjectArrayElement,
  CheckJNI::SetObjectArrayElement,
  CheckJNI::NewBooleanArray,
  CheckJNI::NewByteArray,
  CheckJNI::NewCharArray,
  CheckJNI::NewShortArray,
  CheckJNI::NewIntArray,
  CheckJNI::NewLongArray,
  CheckJNI::NewFloatArray,
  CheckJNI::NewDoubleArray,
  CheckJNI::GetBooleanArrayElements,
  CheckJNI::GetByteArrayElements,
  CheckJNI::GetCharArrayElements,
  CheckJNI::GetShortArrayElements,
  CheckJNI::GetIntArrayElements,
  CheckJNI::GetLongArrayElements,
  CheckJNI::GetFloatArrayElements,
  CheckJNI::GetDoubleArrayElements,
  CheckJNI::ReleaseBooleanArrayElements,
  CheckJNI::ReleaseByteArrayElements,
  CheckJNI::ReleaseCharArrayElements,
  CheckJNI::ReleaseShortArrayElements,
  CheckJNI::ReleaseIntArrayElements,
  CheckJNI::ReleaseLongArrayElements,
  CheckJNI::ReleaseFloatArrayElements,
  CheckJNI::ReleaseDoubleArrayElements,
  CheckJNI::GetBooleanArrayRegion,
  CheckJNI::GetByteArrayRegion,
  CheckJNI::GetCharArrayRegion,
  CheckJNI::GetShortArrayRegion,
  CheckJNI::GetIntArrayRegion,
  CheckJNI::GetLongArrayRegion,
  CheckJNI::GetFloatArrayRegion,
  CheckJNI::GetDoubleArrayRegion,
  CheckJNI::SetBooleanArrayRegion,
  CheckJNI::SetByteArrayRegion,
  CheckJNI::SetCharArrayRegion,
  CheckJNI::SetShortArrayRegion,
  CheckJNI::SetIntArrayRegion,
  CheckJNI::SetLongArrayRegion,
  CheckJNI::SetFloatArrayRegion,
  CheckJNI::SetDoubleArrayRegion,
  CheckJNI::RegisterNatives,
  CheckJNI::UnregisterNatives,
  CheckJNI::MonitorEnter,
  CheckJNI::MonitorExit,
  CheckJNI::GetJavaVM,
  CheckJNI::GetStringRegion,
  CheckJNI::GetStringUTFRegion,
  CheckJNI::GetPrimitiveArrayCritical,
  CheckJNI::ReleasePrimitiveArrayCritical,
  CheckJNI::GetStringCritical,
  CheckJNI::ReleaseStringCritical,
  CheckJNI::NewWeakGlobalRef,
  CheckJNI::DeleteWeakGlobalRef,
  CheckJNI::ExceptionCheck,
  CheckJNI::NewDirectByteBuffer,
  CheckJNI::GetDirectBufferAddress,
  CheckJNI::GetDirectBufferCapacity,
  CheckJNI::GetObjectRefType,
};

const JNINativeInterface* GetCheckJniNativeInterface() {
  return &gCheckNativeInterface;
}

class CheckJII {
public:
  static jint DestroyJavaVM(JavaVM* vm) {
    ScopedCheck sc(vm, false, __FUNCTION__);
    sc.check(true, "v", vm);
    return CHECK_JNI_EXIT("I", baseVm(vm)->DestroyJavaVM(vm));
  }

  static jint AttachCurrentThread(JavaVM* vm, JNIEnv** p_env, void* thr_args) {
    ScopedCheck sc(vm, false, __FUNCTION__);
    sc.check(true, "vpp", vm, p_env, thr_args);
    return CHECK_JNI_EXIT("I", baseVm(vm)->AttachCurrentThread(vm, p_env, thr_args));
  }

  static jint AttachCurrentThreadAsDaemon(JavaVM* vm, JNIEnv** p_env, void* thr_args) {
    ScopedCheck sc(vm, false, __FUNCTION__);
    sc.check(true, "vpp", vm, p_env, thr_args);
    return CHECK_JNI_EXIT("I", baseVm(vm)->AttachCurrentThreadAsDaemon(vm, p_env, thr_args));
  }

  static jint DetachCurrentThread(JavaVM* vm) {
    ScopedCheck sc(vm, true, __FUNCTION__);
    sc.check(true, "v", vm);
    return CHECK_JNI_EXIT("I", baseVm(vm)->DetachCurrentThread(vm));
  }

  static jint GetEnv(JavaVM* vm, void** env, jint version) {
    ScopedCheck sc(vm, true, __FUNCTION__);
    sc.check(true, "v", vm);
    return CHECK_JNI_EXIT("I", baseVm(vm)->GetEnv(vm, env, version));
  }

 private:
  static inline const JNIInvokeInterface* baseVm(JavaVM* vm) {
    return reinterpret_cast<JavaVMExt*>(vm)->unchecked_functions;
  }
};

const JNIInvokeInterface gCheckInvokeInterface = {
  NULL,  // reserved0
  NULL,  // reserved1
  NULL,  // reserved2
  CheckJII::DestroyJavaVM,
  CheckJII::AttachCurrentThread,
  CheckJII::DetachCurrentThread,
  CheckJII::GetEnv,
  CheckJII::AttachCurrentThreadAsDaemon
};

const JNIInvokeInterface* GetCheckJniInvokeInterface() {
  return &gCheckInvokeInterface;
}

}  // namespace art
