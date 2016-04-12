#include "jni_internal.h"
#include "mirror/class-inl.h"
#include "samsung.h"

namespace art {

//----------------------------------------------------
// dalvik.system.PathClassLoader

static jobject PathClassLoader_openNative(JNIEnv* env, jobject javaThis) {
  // Ignore Samsung native method and use the default PathClassLoader constructor
  return nullptr;
}

static JNINativeMethod gMethodsPathClassLoader[] = {
  NATIVE_METHOD(PathClassLoader, openNative, "!(Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)Ldalvik/system/PathClassLoader;"),
};


//----------------------------------------------------
void register_samsung_native_methods(JNIEnv* env) {
  if (!IsSamsungROM())
    return;

  RegisterNativeMethods(env, "dalvik/system/PathClassLoader", gMethodsPathClassLoader, arraysize(gMethodsPathClassLoader));
}

}  // namespace art
