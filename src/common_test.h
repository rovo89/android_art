// Copyright 2011 Google Inc. All Rights Reserved.

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "base64.h"
#include "heap.h"
#include "thread.h"
#include "stringprintf.h"
#include "class_linker.h"
#include "dex_file.h"

#include "unicode/uclean.h"
#include "unicode/uvernum.h"

#include "gtest/gtest.h"

namespace art {

static inline const DexFile* OpenDexFileBase64(const char* base64,
                                               const std::string& location) {
  CHECK(base64 != NULL);
  size_t length;
  byte* dex_bytes = DecodeBase64(base64, &length);
  CHECK(dex_bytes != NULL);
  const DexFile* dex_file = DexFile::OpenPtr(dex_bytes, length, location);
  CHECK(dex_file != NULL);
  return dex_file;
}

class ScratchFile {
 public:
  ScratchFile() {
    std::string filename_template;
    filename_template = getenv("ANDROID_DATA");
    filename_template += "/TmpFile-XXXXXX";
    filename_.reset(strdup(filename_template.c_str()));
    CHECK(filename_ != NULL);
    fd_ = mkstemp(filename_.get());
    CHECK_NE(-1, fd_);
  }

  ~ScratchFile() {
    int unlink_result = unlink(filename_.get());
    CHECK_EQ(0, unlink_result);
    int close_result = close(fd_);
    CHECK_EQ(0, close_result);
  }

  const char* GetFilename() const {
    return filename_.get();
  }

  int GetFd() const {
    return fd_;
  }

 private:
  scoped_ptr_malloc<char> filename_;
  int fd_;
};

class CommonTest : public testing::Test {
 protected:
  virtual void SetUp() {
    is_host_ = getenv("ANDROID_BUILD_TOP") != NULL;

    if (is_host_) {
      // $ANDROID_ROOT is set on the device, but not on the host.
      // We need to set this so that icu4c can find its locale data.
      std::string root;
      root += getenv("ANDROID_BUILD_TOP");
      root += "/out/host/linux-x86";
      setenv("ANDROID_ROOT", root.c_str(), 1);
    }

    android_data_.reset(strdup(is_host_ ? "/tmp/art-data-XXXXXX" : "/sdcard/art-data-XXXXXX"));
    ASSERT_TRUE(android_data_ != NULL);
    const char* android_data_modified = mkdtemp(android_data_.get());
    // note that mkdtemp side effects android_data_ as well
    ASSERT_TRUE(android_data_modified != NULL);
    setenv("ANDROID_DATA", android_data_modified, 1);
    art_cache_.append(android_data_.get());
    art_cache_.append("/art-cache");
    int mkdir_result = mkdir(art_cache_.c_str(), 0700);
    ASSERT_EQ(mkdir_result, 0);

    java_lang_dex_file_.reset(GetLibCoreDex());

    boot_class_path_.push_back(java_lang_dex_file_.get());

    runtime_.reset(Runtime::Create(boot_class_path_));
    ASSERT_TRUE(runtime_ != NULL);
    class_linker_ = runtime_->GetClassLinker();
  }

  virtual void TearDown() {
    const char* android_data = getenv("ANDROID_DATA");
    ASSERT_TRUE(android_data != NULL);
    DIR* dir = opendir(art_cache_.c_str());
    ASSERT_TRUE(dir != NULL);
    while (true) {
      struct dirent entry;
      struct dirent* entry_ptr;
      int readdir_result = readdir_r(dir, &entry, &entry_ptr);
      ASSERT_EQ(0, readdir_result);
      if (entry_ptr == NULL) {
        break;
      }
      if ((strcmp(entry_ptr->d_name, ".") == 0) || (strcmp(entry_ptr->d_name, "..") == 0)) {
        continue;
      }
      std::string filename(art_cache_);
      filename.push_back('/');
      filename.append(entry_ptr->d_name);
      int unlink_result = unlink(filename.c_str());
      ASSERT_EQ(0, unlink_result);
    }
    closedir(dir);
    int rmdir_cache_result = rmdir(art_cache_.c_str());
    ASSERT_EQ(0, rmdir_cache_result);
    int rmdir_data_result = rmdir(android_data_.get());
    ASSERT_EQ(0, rmdir_data_result);

    // icu4c has a fixed 10-element array "gCommonICUDataArray".
    // If we run > 10 tests, we fill that array and u_setCommonData fails.
    // There's a function to clear the array, but it's not public...
    typedef void (*IcuCleanupFn)();
    void* sym = dlsym(RTLD_DEFAULT, "u_cleanup_" U_ICU_VERSION_SHORT);
    CHECK(sym != NULL);
    IcuCleanupFn icu_cleanup_fn = reinterpret_cast<IcuCleanupFn>(sym);
    (*icu_cleanup_fn)();
  }

  std::string GetLibCoreDexFileName() {
    if (is_host_) {
      const char* host_dir = getenv("ANDROID_HOST_OUT");
      CHECK(host_dir != NULL);
      return StringPrintf("%s/framework/core-hostdex.jar", host_dir);
    }
    return std::string("/system/framework/core.jar");
  }

  const DexFile* GetLibCoreDex() {
    std::string libcore_dex_file_name = GetLibCoreDexFileName();
    return DexFile::OpenZip(libcore_dex_file_name);
  }

  const PathClassLoader* AllocPathClassLoader(const DexFile* dex_file) {
    CHECK(dex_file != NULL);
    class_linker_->RegisterDexFile(*dex_file);
    std::vector<const DexFile*> dex_files;
    dex_files.push_back(dex_file);
    return PathClassLoader::Alloc(dex_files);
  }

  const DexFile* OpenTestDexFile(const char* name) {
    CHECK(name != NULL);
    std::string filename;
    if (is_host_) {
      // on the host, just read target dex file
      filename += getenv("ANDROID_PRODUCT_OUT");
    }
    filename += "/system/framework/art-test-dex-";
    filename += name;
    filename += ".jar";
    const DexFile* dex_file = DexFile::OpenZip(filename);
    CHECK(dex_file != NULL) << "Could not open " << filename;
    return dex_file;
  }

  bool is_host_;
  scoped_ptr_malloc<char> android_data_;
  std::string art_cache_;
  scoped_ptr<const DexFile> java_lang_dex_file_;
  std::vector<const DexFile*> boot_class_path_;
  scoped_ptr<Runtime> runtime_;
  ClassLinker* class_linker_;
};

}  // namespace art
