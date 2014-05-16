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

#include <limits.h>

#include "class_linker-inl.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/allocator/dlmalloc.h"
#include "gc/heap.h"
#include "gc/space/dlmalloc_space.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "object_utils.h"
#include "runtime.h"
#include "scoped_fast_native_object_access.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "thread_list.h"
#include "toStringArray.h"

namespace art {

static jfloat VMRuntime_getTargetHeapUtilization(JNIEnv*, jobject) {
  return Runtime::Current()->GetHeap()->GetTargetHeapUtilization();
}

static void VMRuntime_nativeSetTargetHeapUtilization(JNIEnv*, jobject, jfloat target) {
  Runtime::Current()->GetHeap()->SetTargetHeapUtilization(target);
}

static void VMRuntime_startJitCompilation(JNIEnv*, jobject) {
}

static void VMRuntime_disableJitCompilation(JNIEnv*, jobject) {
}

static jobject VMRuntime_newNonMovableArray(JNIEnv* env, jobject, jclass javaElementClass,
                                            jint length) {
  ScopedFastNativeObjectAccess soa(env);
  if (UNLIKELY(length < 0)) {
    ThrowNegativeArraySizeException(length);
    return nullptr;
  }
  mirror::Class* element_class = soa.Decode<mirror::Class*>(javaElementClass);
  if (UNLIKELY(element_class == nullptr)) {
    ThrowNullPointerException(NULL, "element class == null");
    return nullptr;
  }
  Runtime* runtime = Runtime::Current();
  mirror::Class* array_class = runtime->GetClassLinker()->FindArrayClass(soa.Self(), element_class);
  if (UNLIKELY(array_class == nullptr)) {
    return nullptr;
  }
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentNonMovingAllocator();
  mirror::Array* result = mirror::Array::Alloc<true>(soa.Self(), array_class, length,
                                                     array_class->GetComponentSize(), allocator);
  return soa.AddLocalReference<jobject>(result);
}

static jobject VMRuntime_newUnpaddedArray(JNIEnv* env, jobject, jclass javaElementClass,
                                          jint length) {
  ScopedFastNativeObjectAccess soa(env);
  if (UNLIKELY(length < 0)) {
    ThrowNegativeArraySizeException(length);
    return nullptr;
  }
  mirror::Class* element_class = soa.Decode<mirror::Class*>(javaElementClass);
  if (UNLIKELY(element_class == nullptr)) {
    ThrowNullPointerException(NULL, "element class == null");
    return nullptr;
  }
  Runtime* runtime = Runtime::Current();
  mirror::Class* array_class = runtime->GetClassLinker()->FindArrayClass(soa.Self(), element_class);
  if (UNLIKELY(array_class == nullptr)) {
    return nullptr;
  }
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  mirror::Array* result = mirror::Array::Alloc<true>(soa.Self(), array_class, length,
                                                     array_class->GetComponentSize(), allocator,
                                                     true);
  return soa.AddLocalReference<jobject>(result);
}

static jlong VMRuntime_addressOf(JNIEnv* env, jobject, jobject javaArray) {
  if (javaArray == NULL) {  // Most likely allocation failed
    return 0;
  }
  ScopedFastNativeObjectAccess soa(env);
  mirror::Array* array = soa.Decode<mirror::Array*>(javaArray);
  if (!array->IsArrayInstance()) {
    ThrowIllegalArgumentException(NULL, "not an array");
    return 0;
  }
  if (Runtime::Current()->GetHeap()->IsMovableObject(array)) {
    ThrowRuntimeException("Trying to get address of movable array object");
    return 0;
  }
  return reinterpret_cast<uintptr_t>(array->GetRawData(array->GetClass()->GetComponentSize(), 0));
}

static void VMRuntime_clearGrowthLimit(JNIEnv*, jobject) {
  Runtime::Current()->GetHeap()->ClearGrowthLimit();
}

static jboolean VMRuntime_isDebuggerActive(JNIEnv*, jobject) {
  return Dbg::IsDebuggerActive();
}

static jobjectArray VMRuntime_properties(JNIEnv* env, jobject) {
  return toStringArray(env, Runtime::Current()->GetProperties());
}

// This is for backward compatibility with dalvik which returned the
// meaningless "." when no boot classpath or classpath was
// specified. Unfortunately, some tests were using java.class.path to
// lookup relative file locations, so they are counting on this to be
// ".", presumably some applications or libraries could have as well.
static const char* DefaultToDot(const std::string& class_path) {
  return class_path.empty() ? "." : class_path.c_str();
}

static jstring VMRuntime_bootClassPath(JNIEnv* env, jobject) {
  return env->NewStringUTF(DefaultToDot(Runtime::Current()->GetBootClassPathString()));
}

static jstring VMRuntime_classPath(JNIEnv* env, jobject) {
  return env->NewStringUTF(DefaultToDot(Runtime::Current()->GetClassPathString()));
}

static jstring VMRuntime_vmVersion(JNIEnv* env, jobject) {
  return env->NewStringUTF(Runtime::GetVersion());
}

static jstring VMRuntime_vmLibrary(JNIEnv* env, jobject) {
  return env->NewStringUTF(kIsDebugBuild ? "libartd.so" : "libart.so");
}

static void VMRuntime_setTargetSdkVersionNative(JNIEnv* env, jobject, jint targetSdkVersion) {
  // This is the target SDK version of the app we're about to run. It is intended that this a place
  // where workarounds can be enabled.
  // Note that targetSdkVersion may be CUR_DEVELOPMENT (10000).
  // Note that targetSdkVersion may be 0, meaning "current".
  UNUSED(env);
  UNUSED(targetSdkVersion);
}

static void VMRuntime_registerNativeAllocation(JNIEnv* env, jobject, jint bytes) {
  if (UNLIKELY(bytes < 0)) {
    ScopedObjectAccess soa(env);
    ThrowRuntimeException("allocation size negative %d", bytes);
    return;
  }
  Runtime::Current()->GetHeap()->RegisterNativeAllocation(env, bytes);
}

static void VMRuntime_registerNativeFree(JNIEnv* env, jobject, jint bytes) {
  if (UNLIKELY(bytes < 0)) {
    ScopedObjectAccess soa(env);
    ThrowRuntimeException("allocation size negative %d", bytes);
    return;
  }
  Runtime::Current()->GetHeap()->RegisterNativeFree(env, bytes);
}

static void VMRuntime_updateProcessState(JNIEnv* env, jobject, jint process_state) {
  Runtime::Current()->GetHeap()->UpdateProcessState(static_cast<gc::ProcessState>(process_state));
  Runtime::Current()->UpdateProfilerState(process_state);
}

static void VMRuntime_trimHeap(JNIEnv*, jobject) {
  Runtime::Current()->GetHeap()->DoPendingTransitionOrTrim();
}

static void VMRuntime_concurrentGC(JNIEnv* env, jobject) {
  Runtime::Current()->GetHeap()->ConcurrentGC(ThreadForEnv(env));
}

typedef std::map<std::string, mirror::String*> StringTable;

static void PreloadDexCachesStringsCallback(mirror::Object** root, void* arg,
                                            uint32_t /*thread_id*/, RootType /*root_type*/)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  StringTable& table = *reinterpret_cast<StringTable*>(arg);
  mirror::String* string = const_cast<mirror::Object*>(*root)->AsString();
  table[string->ToModifiedUtf8()] = string;
}

// Based on ClassLinker::ResolveString.
static void PreloadDexCachesResolveString(Handle<mirror::DexCache>& dex_cache, uint32_t string_idx,
                                          StringTable& strings)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::String* string = dex_cache->GetResolvedString(string_idx);
  if (string != NULL) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  const char* utf8 = dex_file->StringDataByIdx(string_idx);
  string = strings[utf8];
  if (string == NULL) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved string=" << utf8;
  dex_cache->SetResolvedString(string_idx, string);
}

// Based on ClassLinker::ResolveType.
static void PreloadDexCachesResolveType(mirror::DexCache* dex_cache, uint32_t type_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Class* klass = dex_cache->GetResolvedType(type_idx);
  if (klass != NULL) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  const char* class_name = dex_file->StringByTypeIdx(type_idx);
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  if (class_name[1] == '\0') {
    klass = linker->FindPrimitiveClass(class_name[0]);
  } else {
    klass = linker->LookupClass(class_name, NULL);
  }
  if (klass == NULL) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved klass=" << class_name;
  dex_cache->SetResolvedType(type_idx, klass);
  // Skip uninitialized classes because filled static storage entry implies it is initialized.
  if (!klass->IsInitialized()) {
    // LOG(INFO) << "VMRuntime.preloadDexCaches uninitialized klass=" << class_name;
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches static storage klass=" << class_name;
}

// Based on ClassLinker::ResolveField.
static void PreloadDexCachesResolveField(Handle<mirror::DexCache>& dex_cache,
                                         uint32_t field_idx,
                                         bool is_static)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtField* field = dex_cache->GetResolvedField(field_idx);
  if (field != NULL) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  const DexFile::FieldId& field_id = dex_file->GetFieldId(field_idx);
  Thread* const self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(hs.NewHandle(dex_cache->GetResolvedType(field_id.class_idx_)));
  if (klass.Get() == NULL) {
    return;
  }
  if (is_static) {
    field = mirror::Class::FindStaticField(self, klass, dex_cache.Get(), field_idx);
  } else {
    field = klass->FindInstanceField(dex_cache.Get(), field_idx);
  }
  if (field == NULL) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved field " << PrettyField(field);
  dex_cache->SetResolvedField(field_idx, field);
}

// Based on ClassLinker::ResolveMethod.
static void PreloadDexCachesResolveMethod(Handle<mirror::DexCache>& dex_cache,
                                          uint32_t method_idx,
                                          InvokeType invoke_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* method = dex_cache->GetResolvedMethod(method_idx);
  if (method != NULL) {
    return;
  }
  const DexFile* dex_file = dex_cache->GetDexFile();
  const DexFile::MethodId& method_id = dex_file->GetMethodId(method_idx);
  mirror::Class* klass = dex_cache->GetResolvedType(method_id.class_idx_);
  if (klass == NULL) {
    return;
  }
  switch (invoke_type) {
    case kDirect:
    case kStatic:
      method = klass->FindDirectMethod(dex_cache.Get(), method_idx);
      break;
    case kInterface:
      method = klass->FindInterfaceMethod(dex_cache.Get(), method_idx);
      break;
    case kSuper:
    case kVirtual:
      method = klass->FindVirtualMethod(dex_cache.Get(), method_idx);
      break;
    default:
      LOG(FATAL) << "Unreachable - invocation type: " << invoke_type;
  }
  if (method == NULL) {
    return;
  }
  // LOG(INFO) << "VMRuntime.preloadDexCaches resolved method " << PrettyMethod(method);
  dex_cache->SetResolvedMethod(method_idx, method);
}

struct DexCacheStats {
    uint32_t num_strings;
    uint32_t num_types;
    uint32_t num_fields;
    uint32_t num_methods;
    DexCacheStats() : num_strings(0),
                      num_types(0),
                      num_fields(0),
                      num_methods(0) {}
};

static const bool kPreloadDexCachesEnabled = true;

// Disabled because it takes a long time (extra half second) but
// gives almost no benefit in terms of saving private dirty pages.
static const bool kPreloadDexCachesStrings = false;

static const bool kPreloadDexCachesTypes = true;
static const bool kPreloadDexCachesFieldsAndMethods = true;

static const bool kPreloadDexCachesCollectStats = true;

static void PreloadDexCachesStatsTotal(DexCacheStats* total) {
  if (!kPreloadDexCachesCollectStats) {
    return;
  }

  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  const std::vector<const DexFile*>& boot_class_path = linker->GetBootClassPath();
  for (size_t i = 0; i< boot_class_path.size(); i++) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != NULL);
    total->num_strings += dex_file->NumStringIds();
    total->num_fields += dex_file->NumFieldIds();
    total->num_methods += dex_file->NumMethodIds();
    total->num_types += dex_file->NumTypeIds();
  }
}

static void PreloadDexCachesStatsFilled(DexCacheStats* filled)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (!kPreloadDexCachesCollectStats) {
      return;
  }
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  const std::vector<const DexFile*>& boot_class_path = linker->GetBootClassPath();
  for (size_t i = 0; i< boot_class_path.size(); i++) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != NULL);
    mirror::DexCache* dex_cache = linker->FindDexCache(*dex_file);
    for (size_t i = 0; i < dex_cache->NumStrings(); i++) {
      mirror::String* string = dex_cache->GetResolvedString(i);
      if (string != NULL) {
        filled->num_strings++;
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedTypes(); i++) {
      mirror::Class* klass = dex_cache->GetResolvedType(i);
      if (klass != NULL) {
        filled->num_types++;
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedFields(); i++) {
      mirror::ArtField* field = dex_cache->GetResolvedField(i);
      if (field != NULL) {
        filled->num_fields++;
      }
    }
    for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
      mirror::ArtMethod* method = dex_cache->GetResolvedMethod(i);
      if (method != NULL) {
        filled->num_methods++;
      }
    }
  }
}

// TODO: http://b/11309598 This code was ported over based on the
// Dalvik version. However, ART has similar code in other places such
// as the CompilerDriver. This code could probably be refactored to
// serve both uses.
static void VMRuntime_preloadDexCaches(JNIEnv* env, jobject) {
  if (!kPreloadDexCachesEnabled) {
    return;
  }

  ScopedObjectAccess soa(env);

  DexCacheStats total;
  DexCacheStats before;
  if (kPreloadDexCachesCollectStats) {
    LOG(INFO) << "VMRuntime.preloadDexCaches starting";
    PreloadDexCachesStatsTotal(&total);
    PreloadDexCachesStatsFilled(&before);
  }

  Runtime* runtime = Runtime::Current();
  ClassLinker* linker = runtime->GetClassLinker();
  Thread* self = ThreadForEnv(env);

  // We use a std::map to avoid heap allocating StringObjects to lookup in gDvm.literalStrings
  StringTable strings;
  if (kPreloadDexCachesStrings) {
    runtime->GetInternTable()->VisitRoots(PreloadDexCachesStringsCallback, &strings,
                                          kVisitRootFlagAllRoots);
  }

  const std::vector<const DexFile*>& boot_class_path = linker->GetBootClassPath();
  for (size_t i = 0; i< boot_class_path.size(); i++) {
    const DexFile* dex_file = boot_class_path[i];
    CHECK(dex_file != NULL);
    StackHandleScope<1> hs(self);
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(linker->FindDexCache(*dex_file)));

    if (kPreloadDexCachesStrings) {
      for (size_t i = 0; i < dex_cache->NumStrings(); i++) {
        PreloadDexCachesResolveString(dex_cache, i, strings);
      }
    }

    if (kPreloadDexCachesTypes) {
      for (size_t i = 0; i < dex_cache->NumResolvedTypes(); i++) {
        PreloadDexCachesResolveType(dex_cache.Get(), i);
      }
    }

    if (kPreloadDexCachesFieldsAndMethods) {
      for (size_t class_def_index = 0;
           class_def_index < dex_file->NumClassDefs();
           class_def_index++) {
        const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
        const byte* class_data = dex_file->GetClassData(class_def);
        if (class_data == NULL) {
          continue;
        }
        ClassDataItemIterator it(*dex_file, class_data);
        for (; it.HasNextStaticField(); it.Next()) {
          uint32_t field_idx = it.GetMemberIndex();
          PreloadDexCachesResolveField(dex_cache, field_idx, true);
        }
        for (; it.HasNextInstanceField(); it.Next()) {
          uint32_t field_idx = it.GetMemberIndex();
          PreloadDexCachesResolveField(dex_cache, field_idx, false);
        }
        for (; it.HasNextDirectMethod(); it.Next()) {
          uint32_t method_idx = it.GetMemberIndex();
          InvokeType invoke_type = it.GetMethodInvokeType(class_def);
          PreloadDexCachesResolveMethod(dex_cache, method_idx, invoke_type);
        }
        for (; it.HasNextVirtualMethod(); it.Next()) {
          uint32_t method_idx = it.GetMemberIndex();
          InvokeType invoke_type = it.GetMethodInvokeType(class_def);
          PreloadDexCachesResolveMethod(dex_cache, method_idx, invoke_type);
        }
      }
    }
  }

  if (kPreloadDexCachesCollectStats) {
    DexCacheStats after;
    PreloadDexCachesStatsFilled(&after);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches strings total=%d before=%d after=%d",
                              total.num_strings, before.num_strings, after.num_strings);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches types total=%d before=%d after=%d",
                              total.num_types, before.num_types, after.num_types);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches fields total=%d before=%d after=%d",
                              total.num_fields, before.num_fields, after.num_fields);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches methods total=%d before=%d after=%d",
                              total.num_methods, before.num_methods, after.num_methods);
    LOG(INFO) << StringPrintf("VMRuntime.preloadDexCaches finished");
  }
}


/*
 * This is called by the framework when it knows the application directory and
 * process name.  We use this information to start up the sampling profiler for
 * for ART.
 */
static void VMRuntime_registerAppInfo(JNIEnv* env, jclass, jstring pkgName, jstring appDir, jstring procName) {
  const char *pkgNameChars = env->GetStringUTFChars(pkgName, NULL);
  const char *appDirChars = env->GetStringUTFChars(appDir, NULL);
  const char *procNameChars = env->GetStringUTFChars(procName, NULL);

  std::string profileFile = StringPrintf("/data/dalvik-cache/profiles/%s", pkgNameChars);
  Runtime::Current()->StartProfiler(profileFile.c_str(), procNameChars);
  env->ReleaseStringUTFChars(appDir, appDirChars);
  env->ReleaseStringUTFChars(procName, procNameChars);
  env->ReleaseStringUTFChars(pkgName, pkgNameChars);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(VMRuntime, addressOf, "!(Ljava/lang/Object;)J"),
  NATIVE_METHOD(VMRuntime, bootClassPath, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, classPath, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, clearGrowthLimit, "()V"),
  NATIVE_METHOD(VMRuntime, concurrentGC, "()V"),
  NATIVE_METHOD(VMRuntime, disableJitCompilation, "()V"),
  NATIVE_METHOD(VMRuntime, getTargetHeapUtilization, "()F"),
  NATIVE_METHOD(VMRuntime, isDebuggerActive, "!()Z"),
  NATIVE_METHOD(VMRuntime, nativeSetTargetHeapUtilization, "(F)V"),
  NATIVE_METHOD(VMRuntime, newNonMovableArray, "!(Ljava/lang/Class;I)Ljava/lang/Object;"),
  NATIVE_METHOD(VMRuntime, newUnpaddedArray, "!(Ljava/lang/Class;I)Ljava/lang/Object;"),
  NATIVE_METHOD(VMRuntime, properties, "()[Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, setTargetSdkVersionNative, "(I)V"),
  NATIVE_METHOD(VMRuntime, registerNativeAllocation, "(I)V"),
  NATIVE_METHOD(VMRuntime, registerNativeFree, "(I)V"),
  NATIVE_METHOD(VMRuntime, updateProcessState, "(I)V"),
  NATIVE_METHOD(VMRuntime, startJitCompilation, "()V"),
  NATIVE_METHOD(VMRuntime, trimHeap, "()V"),
  NATIVE_METHOD(VMRuntime, vmVersion, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, vmLibrary, "()Ljava/lang/String;"),
  NATIVE_METHOD(VMRuntime, preloadDexCaches, "()V"),
  NATIVE_METHOD(VMRuntime, registerAppInfo, "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V"),
};

void register_dalvik_system_VMRuntime(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/VMRuntime");
}

}  // namespace art
