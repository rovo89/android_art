/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "common_runtime_test.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <ScopedLocalRef.h>

#include "../../external/icu/icu4c/source/common/unicode/uvernum.h"
#include "base/macros.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "base/stringprintf.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "compiler_callbacks.h"
#include "dex_file.h"
#include "gc_root-inl.h"
#include "gc/heap.h"
#include "gtest/gtest.h"
#include "jni_internal.h"
#include "mirror/class_loader.h"
#include "noop_compiler_callbacks.h"
#include "os.h"
#include "runtime-inl.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "well_known_classes.h"

int main(int argc, char **argv) {
  art::InitLogging(argv);
  LOG(INFO) << "Running main() from common_runtime_test.cc...";
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace art {

ScratchFile::ScratchFile() {
  // ANDROID_DATA needs to be set
  CHECK_NE(static_cast<char*>(nullptr), getenv("ANDROID_DATA")) <<
      "Are you subclassing RuntimeTest?";
  filename_ = getenv("ANDROID_DATA");
  filename_ += "/TmpFile-XXXXXX";
  int fd = mkstemp(&filename_[0]);
  CHECK_NE(-1, fd);
  file_.reset(new File(fd, GetFilename()));
}

ScratchFile::ScratchFile(const ScratchFile& other, const char* suffix) {
  filename_ = other.GetFilename();
  filename_ += suffix;
  int fd = open(filename_.c_str(), O_RDWR | O_CREAT, 0666);
  CHECK_NE(-1, fd);
  file_.reset(new File(fd, GetFilename()));
}

ScratchFile::ScratchFile(File* file) {
  CHECK(file != NULL);
  filename_ = file->GetPath();
  file_.reset(file);
}

ScratchFile::~ScratchFile() {
  Unlink();
}

int ScratchFile::GetFd() const {
  return file_->Fd();
}

void ScratchFile::Unlink() {
  if (!OS::FileExists(filename_.c_str())) {
    return;
  }
  int unlink_result = unlink(filename_.c_str());
  CHECK_EQ(0, unlink_result);
}

CommonRuntimeTest::CommonRuntimeTest() {}
CommonRuntimeTest::~CommonRuntimeTest() {}

void CommonRuntimeTest::SetUpAndroidRoot() {
  if (IsHost()) {
    // $ANDROID_ROOT is set on the device, but not necessarily on the host.
    // But it needs to be set so that icu4c can find its locale data.
    const char* android_root_from_env = getenv("ANDROID_ROOT");
    if (android_root_from_env == nullptr) {
      // Use ANDROID_HOST_OUT for ANDROID_ROOT if it is set.
      const char* android_host_out = getenv("ANDROID_HOST_OUT");
      if (android_host_out != nullptr) {
        setenv("ANDROID_ROOT", android_host_out, 1);
      } else {
        // Build it from ANDROID_BUILD_TOP or cwd
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
      }
    }
    setenv("LD_LIBRARY_PATH", ":", 0);  // Required by java.lang.System.<clinit>.

    // Not set by build server, so default
    if (getenv("ANDROID_HOST_OUT") == nullptr) {
      setenv("ANDROID_HOST_OUT", getenv("ANDROID_ROOT"), 1);
    }
  }
}

void CommonRuntimeTest::SetUpAndroidData(std::string& android_data) {
  // On target, Cannot use /mnt/sdcard because it is mounted noexec, so use subdir of dalvik-cache
  if (IsHost()) {
    const char* tmpdir = getenv("TMPDIR");
    if (tmpdir != nullptr && tmpdir[0] != 0) {
      android_data = tmpdir;
    } else {
      android_data = "/tmp";
    }
  } else {
    android_data = "/data/dalvik-cache";
  }
  android_data += "/art-data-XXXXXX";
  if (mkdtemp(&android_data[0]) == nullptr) {
    PLOG(FATAL) << "mkdtemp(\"" << &android_data[0] << "\") failed";
  }
  setenv("ANDROID_DATA", android_data.c_str(), 1);
}

void CommonRuntimeTest::TearDownAndroidData(const std::string& android_data, bool fail_on_error) {
  if (fail_on_error) {
    ASSERT_EQ(rmdir(android_data.c_str()), 0);
  } else {
    rmdir(android_data.c_str());
  }
}


const DexFile* CommonRuntimeTest::LoadExpectSingleDexFile(const char* location) {
  std::vector<const DexFile*> dex_files;
  std::string error_msg;
  if (!DexFile::Open(location, location, &error_msg, &dex_files)) {
    LOG(FATAL) << "Could not open .dex file '" << location << "': " << error_msg << "\n";
    return nullptr;
  } else {
    CHECK_EQ(1U, dex_files.size()) << "Expected only one dex file in " << location;
    return dex_files[0];
  }
}

void CommonRuntimeTest::SetUp() {
  SetUpAndroidRoot();
  SetUpAndroidData(android_data_);
  dalvik_cache_.append(android_data_.c_str());
  dalvik_cache_.append("/dalvik-cache");
  int mkdir_result = mkdir(dalvik_cache_.c_str(), 0700);
  ASSERT_EQ(mkdir_result, 0);

  std::string error_msg;
  java_lang_dex_file_ = LoadExpectSingleDexFile(GetLibCoreDexFileName().c_str());
  boot_class_path_.push_back(java_lang_dex_file_);

  std::string min_heap_string(StringPrintf("-Xms%zdm", gc::Heap::kDefaultInitialSize / MB));
  std::string max_heap_string(StringPrintf("-Xmx%zdm", gc::Heap::kDefaultMaximumSize / MB));

  callbacks_.reset(new NoopCompilerCallbacks());

  RuntimeOptions options;
  options.push_back(std::make_pair("bootclasspath", &boot_class_path_));
  options.push_back(std::make_pair("-Xcheck:jni", nullptr));
  options.push_back(std::make_pair(min_heap_string.c_str(), nullptr));
  options.push_back(std::make_pair(max_heap_string.c_str(), nullptr));
  options.push_back(std::make_pair("compilercallbacks", callbacks_.get()));
  SetUpRuntimeOptions(&options);
  if (!Runtime::Create(options, false)) {
    LOG(FATAL) << "Failed to create runtime";
    return;
  }
  runtime_.reset(Runtime::Current());
  class_linker_ = runtime_->GetClassLinker();
  class_linker_->FixupDexCaches(runtime_->GetResolutionMethod());
  class_linker_->RunRootClinits();

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

void CommonRuntimeTest::ClearDirectory(const char* dirpath) {
  ASSERT_TRUE(dirpath != nullptr);
  DIR* dir = opendir(dirpath);
  ASSERT_TRUE(dir != nullptr);
  dirent* e;
  struct stat s;
  while ((e = readdir(dir)) != nullptr) {
    if ((strcmp(e->d_name, ".") == 0) || (strcmp(e->d_name, "..") == 0)) {
      continue;
    }
    std::string filename(dirpath);
    filename.push_back('/');
    filename.append(e->d_name);
    int stat_result = lstat(filename.c_str(), &s);
    ASSERT_EQ(0, stat_result) << "unable to stat " << filename;
    if (S_ISDIR(s.st_mode)) {
      ClearDirectory(filename.c_str());
      int rmdir_result = rmdir(filename.c_str());
      ASSERT_EQ(0, rmdir_result) << filename;
    } else {
      int unlink_result = unlink(filename.c_str());
      ASSERT_EQ(0, unlink_result) << filename;
    }
  }
  closedir(dir);
}

void CommonRuntimeTest::TearDown() {
  const char* android_data = getenv("ANDROID_DATA");
  ASSERT_TRUE(android_data != nullptr);
  ClearDirectory(dalvik_cache_.c_str());
  int rmdir_cache_result = rmdir(dalvik_cache_.c_str());
  ASSERT_EQ(0, rmdir_cache_result);
  TearDownAndroidData(android_data_, true);

  // icu4c has a fixed 10-element array "gCommonICUDataArray".
  // If we run > 10 tests, we fill that array and u_setCommonData fails.
  // There's a function to clear the array, but it's not public...
  typedef void (*IcuCleanupFn)();
  void* sym = dlsym(RTLD_DEFAULT, "u_cleanup_" U_ICU_VERSION_SHORT);
  CHECK(sym != nullptr) << dlerror();
  IcuCleanupFn icu_cleanup_fn = reinterpret_cast<IcuCleanupFn>(sym);
  (*icu_cleanup_fn)();

  STLDeleteElements(&opened_dex_files_);

  Runtime::Current()->GetHeap()->VerifyHeap();  // Check for heap corruption after the test
}

std::string CommonRuntimeTest::GetLibCoreDexFileName() {
  return GetDexFileName("core-libart");
}

std::string CommonRuntimeTest::GetDexFileName(const std::string& jar_prefix) {
  if (IsHost()) {
    const char* host_dir = getenv("ANDROID_HOST_OUT");
    CHECK(host_dir != nullptr);
    return StringPrintf("%s/framework/%s-hostdex.jar", host_dir, jar_prefix.c_str());
  }
  return StringPrintf("%s/framework/%s.jar", GetAndroidRoot(), jar_prefix.c_str());
}

std::string CommonRuntimeTest::GetTestAndroidRoot() {
  if (IsHost()) {
    const char* host_dir = getenv("ANDROID_HOST_OUT");
    CHECK(host_dir != nullptr);
    return host_dir;
  }
  return GetAndroidRoot();
}

// Check that for target builds we have ART_TARGET_NATIVETEST_DIR set.
#ifdef ART_TARGET
#ifndef ART_TARGET_NATIVETEST_DIR
#error "ART_TARGET_NATIVETEST_DIR not set."
#endif
// Wrap it as a string literal.
#define ART_TARGET_NATIVETEST_DIR_STRING STRINGIFY(ART_TARGET_NATIVETEST_DIR) "/"
#else
#define ART_TARGET_NATIVETEST_DIR_STRING ""
#endif

std::vector<const DexFile*> CommonRuntimeTest::OpenTestDexFiles(const char* name) {
  CHECK(name != nullptr);
  std::string filename;
  if (IsHost()) {
    filename += getenv("ANDROID_HOST_OUT");
    filename += "/framework/";
  } else {
    filename += ART_TARGET_NATIVETEST_DIR_STRING;
  }
  filename += "art-gtest-";
  filename += name;
  filename += ".jar";
  std::string error_msg;
  std::vector<const DexFile*> dex_files;
  bool success = DexFile::Open(filename.c_str(), filename.c_str(), &error_msg, &dex_files);
  CHECK(success) << "Failed to open '" << filename << "': " << error_msg;
  for (const DexFile* dex_file : dex_files) {
    CHECK_EQ(PROT_READ, dex_file->GetPermissions());
    CHECK(dex_file->IsReadOnly());
  }
  opened_dex_files_.insert(opened_dex_files_.end(), dex_files.begin(), dex_files.end());
  return dex_files;
}

const DexFile* CommonRuntimeTest::OpenTestDexFile(const char* name) {
  std::vector<const DexFile*> vector = OpenTestDexFiles(name);
  EXPECT_EQ(1U, vector.size());
  return vector[0];
}

jobject CommonRuntimeTest::LoadDex(const char* dex_name) {
  std::vector<const DexFile*> dex_files = OpenTestDexFiles(dex_name);
  CHECK_NE(0U, dex_files.size());
  for (const DexFile* dex_file : dex_files) {
    class_linker_->RegisterDexFile(*dex_file);
  }
  ScopedObjectAccessUnchecked soa(Thread::Current());
  ScopedLocalRef<jobject> class_loader_local(soa.Env(),
      soa.Env()->AllocObject(WellKnownClasses::dalvik_system_PathClassLoader));
  jobject class_loader = soa.Env()->NewGlobalRef(class_loader_local.get());
  soa.Self()->SetClassLoaderOverride(soa.Decode<mirror::ClassLoader*>(class_loader_local.get()));
  Runtime::Current()->SetCompileTimeClassPath(class_loader, dex_files);
  return class_loader;
}

CheckJniAbortCatcher::CheckJniAbortCatcher() : vm_(Runtime::Current()->GetJavaVM()) {
  vm_->check_jni_abort_hook = Hook;
  vm_->check_jni_abort_hook_data = &actual_;
}

CheckJniAbortCatcher::~CheckJniAbortCatcher() {
  vm_->check_jni_abort_hook = nullptr;
  vm_->check_jni_abort_hook_data = nullptr;
  EXPECT_TRUE(actual_.empty()) << actual_;
}

void CheckJniAbortCatcher::Check(const char* expected_text) {
  EXPECT_TRUE(actual_.find(expected_text) != std::string::npos) << "\n"
      << "Expected to find: " << expected_text << "\n"
      << "In the output   : " << actual_;
  actual_.clear();
}

void CheckJniAbortCatcher::Hook(void* data, const std::string& reason) {
  // We use += because when we're hooking the aborts like this, multiple problems can be found.
  *reinterpret_cast<std::string*>(data) += reason;
}

}  // namespace art

namespace std {

template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& rhs) {
os << ::art::ToString(rhs);
return os;
}

}  // namespace std
