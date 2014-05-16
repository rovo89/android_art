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

#ifndef ART_RUNTIME_COMMON_RUNTIME_TEST_H_
#define ART_RUNTIME_COMMON_RUNTIME_TEST_H_

#include <dirent.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>

#include "../../external/icu4c/common/unicode/uvernum.h"
#include "base/macros.h"
#include "base/stl_util.h"
#include "base/stringprintf.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "dex_file-inl.h"
#include "entrypoints/entrypoint_utils.h"
#include "gc/heap.h"
#include "gtest/gtest.h"
#include "instruction_set.h"
#include "interpreter/interpreter.h"
#include "mirror/class_loader.h"
#include "noop_compiler_callbacks.h"
#include "oat_file.h"
#include "object_utils.h"
#include "os.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "thread.h"
#include "utils.h"
#include "UniquePtrCompat.h"
#include "verifier/method_verifier.h"
#include "verifier/method_verifier-inl.h"
#include "well_known_classes.h"

namespace art {

class ScratchFile {
 public:
  ScratchFile() {
    // ANDROID_DATA needs to be set
    CHECK_NE(static_cast<char*>(nullptr), getenv("ANDROID_DATA")) <<
        "Are you subclassing RuntimeTest?";
    filename_ = getenv("ANDROID_DATA");
    filename_ += "/TmpFile-XXXXXX";
    int fd = mkstemp(&filename_[0]);
    CHECK_NE(-1, fd);
    file_.reset(new File(fd, GetFilename()));
  }

  ScratchFile(const ScratchFile& other, const char* suffix) {
    filename_ = other.GetFilename();
    filename_ += suffix;
    int fd = open(filename_.c_str(), O_RDWR | O_CREAT, 0666);
    CHECK_NE(-1, fd);
    file_.reset(new File(fd, GetFilename()));
  }

  ~ScratchFile() {
    int unlink_result = unlink(filename_.c_str());
    CHECK_EQ(0, unlink_result);
  }

  const std::string& GetFilename() const {
    return filename_;
  }

  File* GetFile() const {
    return file_.get();
  }

  int GetFd() const {
    return file_->Fd();
  }

 private:
  std::string filename_;
  UniquePtr<File> file_;
};

class CommonRuntimeTest : public testing::Test {
 public:
  static void SetEnvironmentVariables(std::string& android_data) {
    if (IsHost()) {
      // $ANDROID_ROOT is set on the device, but not on the host.
      // We need to set this so that icu4c can find its locale data.
      std::string root;
      const char* android_build_top = getenv("ANDROID_BUILD_TOP");
      if (android_build_top != nullptr) {
        root += android_build_top;
      } else {
        // Not set by build server, so default to current directory
        char* cwd = getcwd(nullptr, 0);
        setenv("ANDROID_BUILD_TOP", cwd, 1);
        root += cwd;
        free(cwd);
      }
#if defined(__linux__)
      root += "/out/host/linux-x86";
#elif defined(__APPLE__)
      root += "/out/host/darwin-x86";
#else
#error unsupported OS
#endif
      setenv("ANDROID_ROOT", root.c_str(), 1);
      setenv("LD_LIBRARY_PATH", ":", 0);  // Required by java.lang.System.<clinit>.

      // Not set by build server, so default
      if (getenv("ANDROID_HOST_OUT") == nullptr) {
        setenv("ANDROID_HOST_OUT", root.c_str(), 1);
      }
    }

    // On target, Cannot use /mnt/sdcard because it is mounted noexec, so use subdir of dalvik-cache
    android_data = (IsHost() ? "/tmp/art-data-XXXXXX" : "/data/dalvik-cache/art-data-XXXXXX");
    if (mkdtemp(&android_data[0]) == nullptr) {
      PLOG(FATAL) << "mkdtemp(\"" << &android_data[0] << "\") failed";
    }
    setenv("ANDROID_DATA", android_data.c_str(), 1);
  }

 protected:
  static bool IsHost() {
    return !kIsTargetBuild;
  }

  virtual void SetUp() {
    SetEnvironmentVariables(android_data_);
    dalvik_cache_.append(android_data_.c_str());
    dalvik_cache_.append("/dalvik-cache");
    int mkdir_result = mkdir(dalvik_cache_.c_str(), 0700);
    ASSERT_EQ(mkdir_result, 0);

    std::string error_msg;
    java_lang_dex_file_ = DexFile::Open(GetLibCoreDexFileName().c_str(),
                                        GetLibCoreDexFileName().c_str(), &error_msg);
    if (java_lang_dex_file_ == nullptr) {
      LOG(FATAL) << "Could not open .dex file '" << GetLibCoreDexFileName() << "': "
          << error_msg << "\n";
    }
    boot_class_path_.push_back(java_lang_dex_file_);

    std::string min_heap_string(StringPrintf("-Xms%zdm", gc::Heap::kDefaultInitialSize / MB));
    std::string max_heap_string(StringPrintf("-Xmx%zdm", gc::Heap::kDefaultMaximumSize / MB));

    Runtime::Options options;
    options.push_back(std::make_pair("bootclasspath", &boot_class_path_));
    options.push_back(std::make_pair("-Xcheck:jni", nullptr));
    options.push_back(std::make_pair(min_heap_string.c_str(), nullptr));
    options.push_back(std::make_pair(max_heap_string.c_str(), nullptr));
    options.push_back(std::make_pair("compilercallbacks", &callbacks_));
    SetUpRuntimeOptions(&options);
    if (!Runtime::Create(options, false)) {
      LOG(FATAL) << "Failed to create runtime";
      return;
    }
    runtime_.reset(Runtime::Current());
    class_linker_ = runtime_->GetClassLinker();
    class_linker_->FixupDexCaches(runtime_->GetResolutionMethod());

    // Runtime::Create acquired the mutator_lock_ that is normally given away when we
    // Runtime::Start, give it away now and then switch to a more managable ScopedObjectAccess.
    Thread::Current()->TransitionFromRunnableToSuspended(kNative);

    // We're back in native, take the opportunity to initialize well known classes.
    WellKnownClasses::Init(Thread::Current()->GetJniEnv());

    // Create the heap thread pool so that the GC runs in parallel for tests. Normally, the thread
    // pool is created by the runtime.
    runtime_->GetHeap()->CreateThreadPool();
    runtime_->GetHeap()->VerifyHeap();  // Check for heap corruption before the test
  }

  // Allow subclases such as CommonCompilerTest to add extra options.
  virtual void SetUpRuntimeOptions(Runtime::Options *options) {}

  virtual void TearDown() {
    const char* android_data = getenv("ANDROID_DATA");
    ASSERT_TRUE(android_data != nullptr);
    DIR* dir = opendir(dalvik_cache_.c_str());
    ASSERT_TRUE(dir != nullptr);
    dirent* e;
    while ((e = readdir(dir)) != nullptr) {
      if ((strcmp(e->d_name, ".") == 0) || (strcmp(e->d_name, "..") == 0)) {
        continue;
      }
      std::string filename(dalvik_cache_);
      filename.push_back('/');
      filename.append(e->d_name);
      int unlink_result = unlink(filename.c_str());
      ASSERT_EQ(0, unlink_result);
    }
    closedir(dir);
    int rmdir_cache_result = rmdir(dalvik_cache_.c_str());
    ASSERT_EQ(0, rmdir_cache_result);
    int rmdir_data_result = rmdir(android_data_.c_str());
    ASSERT_EQ(0, rmdir_data_result);

    // icu4c has a fixed 10-element array "gCommonICUDataArray".
    // If we run > 10 tests, we fill that array and u_setCommonData fails.
    // There's a function to clear the array, but it's not public...
    typedef void (*IcuCleanupFn)();
    void* sym = dlsym(RTLD_DEFAULT, "u_cleanup_" U_ICU_VERSION_SHORT);
    CHECK(sym != nullptr);
    IcuCleanupFn icu_cleanup_fn = reinterpret_cast<IcuCleanupFn>(sym);
    (*icu_cleanup_fn)();

    STLDeleteElements(&opened_dex_files_);

    Runtime::Current()->GetHeap()->VerifyHeap();  // Check for heap corruption after the test
  }

  std::string GetLibCoreDexFileName() {
    return GetDexFileName("core-libart");
  }

  std::string GetDexFileName(const std::string& jar_prefix) {
    if (IsHost()) {
      const char* host_dir = getenv("ANDROID_HOST_OUT");
      CHECK(host_dir != nullptr);
      return StringPrintf("%s/framework/%s-hostdex.jar", host_dir, jar_prefix.c_str());
    }
    return StringPrintf("%s/framework/%s.jar", GetAndroidRoot(), jar_prefix.c_str());
  }

  std::string GetTestAndroidRoot() {
    if (IsHost()) {
      const char* host_dir = getenv("ANDROID_HOST_OUT");
      CHECK(host_dir != nullptr);
      return host_dir;
    }
    return GetAndroidRoot();
  }

  const DexFile* OpenTestDexFile(const char* name) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(name != nullptr);
    std::string filename;
    if (IsHost()) {
      filename += getenv("ANDROID_HOST_OUT");
      filename += "/framework/";
    } else {
#ifdef __LP64__
      filename += "/data/nativetest/art64/";
#else
      filename += "/data/nativetest/art/";
#endif
    }
    filename += "art-test-dex-";
    filename += name;
    filename += ".jar";
    std::string error_msg;
    const DexFile* dex_file = DexFile::Open(filename.c_str(), filename.c_str(), &error_msg);
    CHECK(dex_file != nullptr) << "Failed to open '" << filename << "': " << error_msg;
    CHECK_EQ(PROT_READ, dex_file->GetPermissions());
    CHECK(dex_file->IsReadOnly());
    opened_dex_files_.push_back(dex_file);
    return dex_file;
  }

  jobject LoadDex(const char* dex_name) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const DexFile* dex_file = OpenTestDexFile(dex_name);
    CHECK(dex_file != nullptr);
    class_linker_->RegisterDexFile(*dex_file);
    std::vector<const DexFile*> class_path;
    class_path.push_back(dex_file);
    ScopedObjectAccessUnchecked soa(Thread::Current());
    ScopedLocalRef<jobject> class_loader_local(soa.Env(),
        soa.Env()->AllocObject(WellKnownClasses::dalvik_system_PathClassLoader));
    jobject class_loader = soa.Env()->NewGlobalRef(class_loader_local.get());
    soa.Self()->SetClassLoaderOverride(soa.Decode<mirror::ClassLoader*>(class_loader_local.get()));
    Runtime::Current()->SetCompileTimeClassPath(class_loader, class_path);
    return class_loader;
  }

  std::string android_data_;
  std::string dalvik_cache_;
  const DexFile* java_lang_dex_file_;  // owned by runtime_
  std::vector<const DexFile*> boot_class_path_;
  UniquePtr<Runtime> runtime_;
  // Owned by the runtime
  ClassLinker* class_linker_;

 private:
  NoopCompilerCallbacks callbacks_;
  std::vector<const DexFile*> opened_dex_files_;
};

// Sets a CheckJni abort hook to catch failures. Note that this will cause CheckJNI to carry on
// rather than aborting, so be careful!
class CheckJniAbortCatcher {
 public:
  CheckJniAbortCatcher() : vm_(Runtime::Current()->GetJavaVM()) {
    vm_->check_jni_abort_hook = Hook;
    vm_->check_jni_abort_hook_data = &actual_;
  }

  ~CheckJniAbortCatcher() {
    vm_->check_jni_abort_hook = nullptr;
    vm_->check_jni_abort_hook_data = nullptr;
    EXPECT_TRUE(actual_.empty()) << actual_;
  }

  void Check(const char* expected_text) {
    EXPECT_TRUE(actual_.find(expected_text) != std::string::npos) << "\n"
        << "Expected to find: " << expected_text << "\n"
        << "In the output   : " << actual_;
    actual_.clear();
  }

 private:
  static void Hook(void* data, const std::string& reason) {
    // We use += because when we're hooking the aborts like this, multiple problems can be found.
    *reinterpret_cast<std::string*>(data) += reason;
  }

  JavaVMExt* vm_;
  std::string actual_;

  DISALLOW_COPY_AND_ASSIGN(CheckJniAbortCatcher);
};

// TODO: These tests were disabled for portable when we went to having
// MCLinker link LLVM ELF output because we no longer just have code
// blobs in memory. We'll need to dlopen to load and relocate
// temporary output to resurrect these tests.
#define TEST_DISABLED_FOR_PORTABLE() \
  if (kUsePortableCompiler) { \
    printf("WARNING: TEST DISABLED FOR PORTABLE\n"); \
    return; \
  }

// TODO: When heap reference poisoning works with the compiler, get rid of this.
#define TEST_DISABLED_FOR_HEAP_REFERENCE_POISONING() \
  if (kPoisonHeapReferences) { \
    printf("WARNING: TEST DISABLED FOR HEAP REFERENCE POISONING\n"); \
    return; \
  }

}  // namespace art

namespace std {

// TODO: isn't gtest supposed to be able to print STL types for itself?
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& rhs) {
  os << ::art::ToString(rhs);
  return os;
}

}  // namespace std

#endif  // ART_RUNTIME_COMMON_RUNTIME_TEST_H_
