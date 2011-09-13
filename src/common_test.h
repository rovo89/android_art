// Copyright 2011 Google Inc. All Rights Reserved.

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "UniquePtr.h"
#include "base64.h"
#include "class_linker.h"
#include "class_loader.h"
#include "compiler.h"
#include "constants.h"
#include "dex_file.h"
#include "gtest/gtest.h"
#include "heap.h"
#include "runtime.h"
#include "stringprintf.h"
#include "thread.h"
#include "unicode/uclean.h"
#include "unicode/uvernum.h"

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
    filename_ = getenv("ANDROID_DATA");
    filename_ += "/TmpFile-XXXXXX";
    fd_ = mkstemp(&filename_[0]);
    CHECK_NE(-1, fd_);
  }

  ~ScratchFile() {
    int unlink_result = unlink(filename_.c_str());
    CHECK_EQ(0, unlink_result);
    int close_result = close(fd_);
    CHECK_EQ(0, close_result);
  }

  const char* GetFilename() const {
    return filename_.c_str();
  }

  int GetFd() const {
    return fd_;
  }

 private:
  std::string filename_;
  int fd_;
};

class CommonTest : public testing::Test {
 public:
  static void MakeExecutable(const ByteArray* byte_array) {
    uintptr_t data = reinterpret_cast<uintptr_t>(byte_array->GetData());
    uintptr_t base = RoundDown(data, kPageSize);
    uintptr_t limit = RoundUp(data + byte_array->GetLength(), kPageSize);
    uintptr_t len = limit - base;
    int result = mprotect(reinterpret_cast<void*>(base), len, PROT_READ | PROT_WRITE | PROT_EXEC);
    CHECK_EQ(result, 0);
  }

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

    // On target, Cannot use /mnt/sdcard because it is mounted noexec, so use subdir of art-cache
    android_data_ = (is_host_ ? "/tmp/art-data-XXXXXX" : "/data/art-cache/art-data-XXXXXX");
    if (mkdtemp(&android_data_[0]) == NULL) {
      PLOG(FATAL) << "mkdtemp(\"" << &android_data_[0] << "\") failed";
    }
    setenv("ANDROID_DATA", android_data_.c_str(), 1);
    art_cache_.append(android_data_.c_str());
    art_cache_.append("/art-cache");
    int mkdir_result = mkdir(art_cache_.c_str(), 0700);
    ASSERT_EQ(mkdir_result, 0);

    java_lang_dex_file_.reset(GetLibCoreDex());

    boot_class_path_.push_back(java_lang_dex_file_.get());

    Runtime::Options options;
    options.push_back(std::make_pair("bootclasspath", &boot_class_path_));
    options.push_back(std::make_pair("-Xcheck:jni", reinterpret_cast<void*>(NULL)));
    runtime_.reset(Runtime::Create(options, false));
    ASSERT_TRUE(runtime_.get() != NULL);
    runtime_->Start();
    class_linker_ = runtime_->GetClassLinker();

#if defined(__i386__)
    runtime_->SetJniStubArray(JniCompiler::CreateJniStub(kX86));
    compiler_.reset(new Compiler(kX86));
#elif defined(__arm__)
    runtime_->SetJniStubArray(JniCompiler::CreateJniStub(kThumb2));
    compiler_.reset(new Compiler(kThumb2));
#endif

    Heap::VerifyHeap();  // Check for heap corruption before the test
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
    int rmdir_data_result = rmdir(android_data_.c_str());
    ASSERT_EQ(0, rmdir_data_result);

    // icu4c has a fixed 10-element array "gCommonICUDataArray".
    // If we run > 10 tests, we fill that array and u_setCommonData fails.
    // There's a function to clear the array, but it's not public...
    typedef void (*IcuCleanupFn)();
    void* sym = dlsym(RTLD_DEFAULT, "u_cleanup_" U_ICU_VERSION_SHORT);
    CHECK(sym != NULL);
    IcuCleanupFn icu_cleanup_fn = reinterpret_cast<IcuCleanupFn>(sym);
    (*icu_cleanup_fn)();

    compiler_.reset();

    Heap::VerifyHeap();  // Check for heap corruption after the test
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
    return DexFile::OpenZip(libcore_dex_file_name, "");
  }

  uint32_t FindTypeIdxByDescriptor(const DexFile& dex_file, const StringPiece& descriptor) {
    for (size_t i = 0; i < dex_file.NumTypeIds(); i++) {
      const DexFile::TypeId& type_id = dex_file.GetTypeId(i);
      if (descriptor == dex_file.GetTypeDescriptor(type_id)) {
        return i;
      }
    }
    CHECK(false) << "Failed to find type index for " << descriptor;
    return 0;
  }

  uint32_t FindFieldIdxByDescriptorAndName(const DexFile& dex_file,
                                           const StringPiece& class_descriptor,
                                           const StringPiece& field_name) {
    for (size_t i = 0; i < dex_file.NumFieldIds(); i++) {
      const DexFile::FieldId& field_id = dex_file.GetFieldId(i);
      if (class_descriptor == dex_file.GetFieldClassDescriptor(field_id)
          && field_name == dex_file.GetFieldName(field_id)) {
        return i;
      }
    }
    CHECK(false) << "Failed to find field index for " << class_descriptor << " " << field_name;
    return 0;
  }

  const ClassLoader* AllocPathClassLoader(const DexFile* dex_file) {
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
    const DexFile* dex_file = DexFile::OpenZip(filename, "");
    CHECK(dex_file != NULL) << "Failed to open " << filename;
    return dex_file;
  }

  const ClassLoader* LoadDex(const char* dex_name) {
    const DexFile* dex_file = OpenTestDexFile(dex_name);
    CHECK(dex_file != NULL);
    loaded_dex_files_.push_back(dex_file);
    class_linker_->RegisterDexFile(*dex_file);
    std::vector<const DexFile*> class_path;
    class_path.push_back(dex_file);
    const ClassLoader* class_loader = PathClassLoader::Alloc(class_path);
    CHECK(class_loader != NULL);
    Thread::Current()->SetClassLoaderOverride(class_loader);
    return class_loader;
  }

  std::string ConvertClassNameToClassDescriptor(const char* class_name) {
    std::string desc;
    desc += "L";
    desc += class_name;
    desc += ";";
    std::replace(desc.begin(), desc.end(), '.', '/');
    return desc;
  }

  void CompileMethod(Method* method) {
    CHECK(method != NULL);
    compiler_->CompileOne(method);
    MakeExecutable(runtime_->GetJniStubArray());
    MakeExecutable(method->GetCodeArray());
    MakeExecutable(method->GetInvokeStubArray());
  }

  void CompileDirectMethod(const ClassLoader* class_loader,
                           const char* class_name,
                           const char* method_name,
                           const char* signature) {
    std::string class_descriptor = ConvertClassNameToClassDescriptor(class_name);
    Class* klass = class_linker_->FindClass(class_descriptor, class_loader);
    CHECK(klass != NULL) << "Class not found " << class_name;
    Method* method = klass->FindDirectMethod(method_name, signature);
    CHECK(method != NULL) << "Direct method not found: "
                          << class_name << "." << method_name << signature;
    CompileMethod(method);
  }

  void CompileVirtualMethod(const ClassLoader* class_loader,
                            const char* class_name,
                            const char* method_name,
                            const char* signature) {
    std::string class_descriptor = ConvertClassNameToClassDescriptor(class_name);
    Class* klass = class_linker_->FindClass(class_descriptor, class_loader);
    CHECK(klass != NULL) << "Class not found " << class_name;
    Method* method = klass->FindVirtualMethod(method_name, signature);
    CHECK(method != NULL) << "Virtual method not found: "
                          << class_name << "." << method_name << signature;
    CompileMethod(method);
  }

  bool is_host_;
  std::string android_data_;
  std::string art_cache_;
  UniquePtr<const DexFile> java_lang_dex_file_;
  std::vector<const DexFile*> boot_class_path_;
  UniquePtr<Runtime> runtime_;
  // Owned by the runtime
  ClassLinker* class_linker_;
  UniquePtr<Compiler> compiler_;

 private:
  std::vector<const DexFile*> loaded_dex_files_;
};

}  // namespace art

namespace std {

// TODO: isn't gtest supposed to be able to print STL types for itself?
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& rhs) {
  os << "[";
  for (size_t i = 0; i < rhs.size(); ++i) {
    os << rhs[i];
    if (i < rhs.size() - 1) {
      os << ", ";
    }
  }
  os << "]";
  return os;
}

}  // namespace std
