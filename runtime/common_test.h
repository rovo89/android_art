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

#ifndef ART_RUNTIME_COMMON_TEST_H_
#define ART_RUNTIME_COMMON_TEST_H_

#include <dirent.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>

#include "../../external/icu4c/common/unicode/uvernum.h"
#include "../compiler/compiler_backend.h"
#include "../compiler/dex/quick/dex_file_to_method_inliner_map.h"
#include "../compiler/dex/verification_results.h"
#include "../compiler/driver/compiler_driver.h"
#include "base/macros.h"
#include "base/stl_util.h"
#include "base/stringprintf.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "compiler_callbacks.h"
#include "dex_file-inl.h"
#include "entrypoints/entrypoint_utils.h"
#include "gc/heap.h"
#include "gtest/gtest.h"
#include "instruction_set.h"
#include "interpreter/interpreter.h"
#include "mirror/class_loader.h"
#include "oat_file.h"
#include "object_utils.h"
#include "os.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "thread.h"
#include "utils.h"
#include "UniquePtr.h"
#include "verifier/method_verifier.h"
#include "verifier/method_verifier-inl.h"
#include "well_known_classes.h"

namespace art {

static const byte kBase64Map[256] = {
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255,  62, 255, 255, 255,  63,
  52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255,
  255, 254, 255, 255, 255,   0,   1,   2,   3,   4,   5,   6,
    7,   8,   9,  10,  11,  12,  13,  14,  15,  16,  17,  18,  // NOLINT
   19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255, 255,  // NOLINT
  255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,
   37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  // NOLINT
   49,  50,  51, 255, 255, 255, 255, 255, 255, 255, 255, 255,  // NOLINT
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255
};

byte* DecodeBase64(const char* src, size_t* dst_size) {
  std::vector<byte> tmp;
  uint32_t t = 0, y = 0;
  int g = 3;
  for (size_t i = 0; src[i] != '\0'; ++i) {
    byte c = kBase64Map[src[i] & 0xFF];
    if (c == 255) continue;
    // the final = symbols are read and used to trim the remaining bytes
    if (c == 254) {
      c = 0;
      // prevent g < 0 which would potentially allow an overflow later
      if (--g < 0) {
        *dst_size = 0;
        return NULL;
      }
    } else if (g != 3) {
      // we only allow = to be at the end
      *dst_size = 0;
      return NULL;
    }
    t = (t << 6) | c;
    if (++y == 4) {
      tmp.push_back((t >> 16) & 255);
      if (g > 1) {
        tmp.push_back((t >> 8) & 255);
      }
      if (g > 2) {
        tmp.push_back(t & 255);
      }
      y = t = 0;
    }
  }
  if (y != 0) {
    *dst_size = 0;
    return NULL;
  }
  UniquePtr<byte[]> dst(new byte[tmp.size()]);
  if (dst_size != NULL) {
    *dst_size = tmp.size();
  } else {
    *dst_size = 0;
  }
  std::copy(tmp.begin(), tmp.end(), dst.get());
  return dst.release();
}

class ScratchFile {
 public:
  ScratchFile() {
    filename_ = getenv("ANDROID_DATA");
    filename_ += "/TmpFile-XXXXXX";
    int fd = mkstemp(&filename_[0]);
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

#if defined(__arm__)

#include <sys/ucontext.h>

// A signal handler called when have an illegal instruction.  We record the fact in
// a global boolean and then increment the PC in the signal context to return to
// the next instruction.  We know the instruction is an sdiv (4 bytes long).
static void baddivideinst(int signo, siginfo *si, void *data) {
  (void)signo;
  (void)si;
  struct ucontext *uc = (struct ucontext *)data;
  struct sigcontext *sc = &uc->uc_mcontext;
  sc->arm_r0 = 0;     // set R0 to #0 to signal error
  sc->arm_pc += 4;    // skip offending instruction
}

// This is in arch/arm/arm_sdiv.S.  It does the following:
// mov r1,#1
// sdiv r0,r1,r1
// bx lr
//
// the result will be the value 1 if sdiv is supported.  If it is not supported
// a SIGILL signal will be raised and the signal handler (baddivideinst) called.
// The signal handler sets r0 to #0 and then increments pc beyond the failed instruction.
// Thus if the instruction is not supported, the result of this function will be #0

extern "C" bool CheckForARMSDIVInstruction();

static InstructionSetFeatures GuessInstructionFeatures() {
  InstructionSetFeatures f;

  // Uncomment this for processing of /proc/cpuinfo.
  if (false) {
    // Look in /proc/cpuinfo for features we need.  Only use this when we can guarantee that
    // the kernel puts the appropriate feature flags in here.  Sometimes it doesn't.
    std::ifstream in("/proc/cpuinfo");
    if (in) {
      while (!in.eof()) {
        std::string line;
        std::getline(in, line);
        if (!in.eof()) {
          if (line.find("Features") != std::string::npos) {
            if (line.find("idivt") != std::string::npos) {
              f.SetHasDivideInstruction(true);
            }
          }
        }
        in.close();
      }
    } else {
      LOG(INFO) << "Failed to open /proc/cpuinfo";
    }
  }

  // See if have a sdiv instruction.  Register a signal handler and try to execute
  // an sdiv instruction.  If we get a SIGILL then it's not supported.  We can't use
  // the /proc/cpuinfo method for this because Krait devices don't always put the idivt
  // feature in the list.
  struct sigaction sa, osa;
  sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO;
  sa.sa_sigaction = baddivideinst;
  sigaction(SIGILL, &sa, &osa);

  if (CheckForARMSDIVInstruction()) {
    f.SetHasDivideInstruction(true);
  }

  // Restore the signal handler.
  sigaction(SIGILL, &osa, NULL);

  // Other feature guesses in here.
  return f;
}

#endif

// Given a set of instruction features from the build, parse it.  The
// input 'str' is a comma separated list of feature names.  Parse it and
// return the InstructionSetFeatures object.
static InstructionSetFeatures ParseFeatureList(std::string str) {
  InstructionSetFeatures result;
  typedef std::vector<std::string> FeatureList;
  FeatureList features;
  Split(str, ',', features);
  for (FeatureList::iterator i = features.begin(); i != features.end(); i++) {
    std::string feature = Trim(*i);
    if (feature == "default") {
      // Nothing to do.
    } else if (feature == "div") {
      // Supports divide instruction.
      result.SetHasDivideInstruction(true);
    } else if (feature == "nodiv") {
      // Turn off support for divide instruction.
      result.SetHasDivideInstruction(false);
    } else {
      LOG(FATAL) << "Unknown instruction set feature: '" << feature << "'";
    }
  }
  // Others...
  return result;
}

class CommonTest : public testing::Test {
 public:
  static void MakeExecutable(const std::vector<uint8_t>& code) {
    CHECK_NE(code.size(), 0U);
    MakeExecutable(&code[0], code.size());
  }

  // Create an OatMethod based on pointers (for unit tests).
  OatFile::OatMethod CreateOatMethod(const void* code,
                                     const size_t frame_size_in_bytes,
                                     const uint32_t core_spill_mask,
                                     const uint32_t fp_spill_mask,
                                     const uint8_t* mapping_table,
                                     const uint8_t* vmap_table,
                                     const uint8_t* gc_map) {
    const byte* base;
    uint32_t code_offset, mapping_table_offset, vmap_table_offset, gc_map_offset;
    if (mapping_table == nullptr && vmap_table == nullptr && gc_map == nullptr) {
      base = reinterpret_cast<const byte*>(code);  // Base of data points at code.
      base -= kPointerSize;  // Move backward so that code_offset != 0.
      code_offset = kPointerSize;
      mapping_table_offset = 0;
      vmap_table_offset = 0;
      gc_map_offset = 0;
    } else {
      // TODO: 64bit support.
      base = nullptr;  // Base of data in oat file, ie 0.
      code_offset = PointerToLowMemUInt32(code);
      mapping_table_offset = PointerToLowMemUInt32(mapping_table);
      vmap_table_offset = PointerToLowMemUInt32(vmap_table);
      gc_map_offset = PointerToLowMemUInt32(gc_map);
    }
    return OatFile::OatMethod(base,
                              code_offset,
                              frame_size_in_bytes,
                              core_spill_mask,
                              fp_spill_mask,
                              mapping_table_offset,
                              vmap_table_offset,
                              gc_map_offset);
  }

  void MakeExecutable(mirror::ArtMethod* method) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(method != nullptr);

    const CompiledMethod* compiled_method = nullptr;
    if (!method->IsAbstract()) {
      mirror::DexCache* dex_cache = method->GetDeclaringClass()->GetDexCache();
      const DexFile& dex_file = *dex_cache->GetDexFile();
      compiled_method =
          compiler_driver_->GetCompiledMethod(MethodReference(&dex_file,
                                                              method->GetDexMethodIndex()));
    }
    if (compiled_method != nullptr) {
      const std::vector<uint8_t>* code = compiled_method->GetQuickCode();
      if (code == nullptr) {
        code = compiled_method->GetPortableCode();
      }
      MakeExecutable(*code);
      const void* method_code = CompiledMethod::CodePointer(&(*code)[0],
                                                            compiled_method->GetInstructionSet());
      LOG(INFO) << "MakeExecutable " << PrettyMethod(method) << " code=" << method_code;
      OatFile::OatMethod oat_method = CreateOatMethod(method_code,
                                                      compiled_method->GetFrameSizeInBytes(),
                                                      compiled_method->GetCoreSpillMask(),
                                                      compiled_method->GetFpSpillMask(),
                                                      &compiled_method->GetMappingTable()[0],
                                                      &compiled_method->GetVmapTable()[0],
                                                      NULL);
      oat_method.LinkMethod(method);
      method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
    } else {
      // No code? You must mean to go into the interpreter.
      const void* method_code = kUsePortableCompiler ? GetPortableToInterpreterBridge()
                                                     : GetQuickToInterpreterBridge();
      OatFile::OatMethod oat_method = CreateOatMethod(method_code,
                                                      kStackAlignment,
                                                      0,
                                                      0,
                                                      NULL,
                                                      NULL,
                                                      NULL);
      oat_method.LinkMethod(method);
      method->SetEntryPointFromInterpreter(interpreter::artInterpreterToInterpreterBridge);
    }
    // Create bridges to transition between different kinds of compiled bridge.
    if (method->GetEntryPointFromPortableCompiledCode() == nullptr) {
      method->SetEntryPointFromPortableCompiledCode(GetPortableToQuickBridge());
    } else {
      CHECK(method->GetEntryPointFromQuickCompiledCode() == nullptr);
      method->SetEntryPointFromQuickCompiledCode(GetQuickToPortableBridge());
      method->SetIsPortableCompiled();
    }
  }

  static void MakeExecutable(const void* code_start, size_t code_length) {
    CHECK(code_start != NULL);
    CHECK_NE(code_length, 0U);
    uintptr_t data = reinterpret_cast<uintptr_t>(code_start);
    uintptr_t base = RoundDown(data, kPageSize);
    uintptr_t limit = RoundUp(data + code_length, kPageSize);
    uintptr_t len = limit - base;
    int result = mprotect(reinterpret_cast<void*>(base), len, PROT_READ | PROT_WRITE | PROT_EXEC);
    CHECK_EQ(result, 0);

    // Flush instruction cache
    // Only uses __builtin___clear_cache if GCC >= 4.3.3
#if GCC_VERSION >= 40303
    __builtin___clear_cache(reinterpret_cast<void*>(base), reinterpret_cast<void*>(base + len));
#else
    LOG(FATAL) << "UNIMPLEMENTED: cache flush";
#endif
  }

  static void SetEnvironmentVariables(std::string& android_data) {
    if (IsHost()) {
      // $ANDROID_ROOT is set on the device, but not on the host.
      // We need to set this so that icu4c can find its locale data.
      std::string root;
      root += getenv("ANDROID_BUILD_TOP");
#if defined(__linux__)
      root += "/out/host/linux-x86";
#elif defined(__APPLE__)
      root += "/out/host/darwin-x86";
#else
#error unsupported OS
#endif
      setenv("ANDROID_ROOT", root.c_str(), 1);
      setenv("LD_LIBRARY_PATH", ":", 0);  // Required by java.lang.System.<clinit>.
    }

    // On target, Cannot use /mnt/sdcard because it is mounted noexec, so use subdir of dalvik-cache
    android_data = (IsHost() ? "/tmp/art-data-XXXXXX" : "/data/dalvik-cache/art-data-XXXXXX");
    if (mkdtemp(&android_data[0]) == NULL) {
      PLOG(FATAL) << "mkdtemp(\"" << &android_data[0] << "\") failed";
    }
    setenv("ANDROID_DATA", android_data.c_str(), 1);
  }

  void MakeExecutable(mirror::ClassLoader* class_loader, const char* class_name)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::string class_descriptor(DotToDescriptor(class_name));
    SirtRef<mirror::ClassLoader> loader(Thread::Current(), class_loader);
    mirror::Class* klass = class_linker_->FindClass(class_descriptor.c_str(), loader);
    CHECK(klass != NULL) << "Class not found " << class_name;
    for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
      MakeExecutable(klass->GetDirectMethod(i));
    }
    for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
      MakeExecutable(klass->GetVirtualMethod(i));
    }
  }

 protected:
  static bool IsHost() {
    return (getenv("ANDROID_BUILD_TOP") != NULL);
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
    if (java_lang_dex_file_ == NULL) {
      LOG(FATAL) << "Could not open .dex file '" << GetLibCoreDexFileName() << "': "
          << error_msg << "\n";
    }
    boot_class_path_.push_back(java_lang_dex_file_);

    std::string min_heap_string(StringPrintf("-Xms%zdm", gc::Heap::kDefaultInitialSize / MB));
    std::string max_heap_string(StringPrintf("-Xmx%zdm", gc::Heap::kDefaultMaximumSize / MB));

    // TODO: make selectable
    CompilerBackend::Kind compiler_backend = kUsePortableCompiler
        ? CompilerBackend::kPortable
        : CompilerBackend::kQuick;

    verification_results_.reset(new VerificationResults);
    method_inliner_map_.reset(new DexFileToMethodInlinerMap);
    callbacks_.Reset(verification_results_.get(), method_inliner_map_.get());
    Runtime::Options options;
    options.push_back(std::make_pair("compilercallbacks", static_cast<CompilerCallbacks*>(&callbacks_)));
    options.push_back(std::make_pair("bootclasspath", &boot_class_path_));
    options.push_back(std::make_pair("-Xcheck:jni", reinterpret_cast<void*>(NULL)));
    options.push_back(std::make_pair(min_heap_string.c_str(), reinterpret_cast<void*>(NULL)));
    options.push_back(std::make_pair(max_heap_string.c_str(), reinterpret_cast<void*>(NULL)));
    if (!Runtime::Create(options, false)) {
      LOG(FATAL) << "Failed to create runtime";
      return;
    }
    runtime_.reset(Runtime::Current());
    // Runtime::Create acquired the mutator_lock_ that is normally given away when we Runtime::Start,
    // give it away now and then switch to a more managable ScopedObjectAccess.
    Thread::Current()->TransitionFromRunnableToSuspended(kNative);
    {
      ScopedObjectAccess soa(Thread::Current());
      ASSERT_TRUE(runtime_.get() != NULL);
      class_linker_ = runtime_->GetClassLinker();

      InstructionSet instruction_set = kNone;

      // Take the default set of instruction features from the build.
      InstructionSetFeatures instruction_set_features =
          ParseFeatureList(STRINGIFY(ART_DEFAULT_INSTRUCTION_SET_FEATURES));

#if defined(__arm__)
      instruction_set = kThumb2;
      InstructionSetFeatures runtime_features = GuessInstructionFeatures();

      // for ARM, do a runtime check to make sure that the features we are passed from
      // the build match the features we actually determine at runtime.
      ASSERT_EQ(instruction_set_features, runtime_features);
#elif defined(__mips__)
      instruction_set = kMips;
#elif defined(__i386__)
      instruction_set = kX86;
#elif defined(__x86_64__)
      instruction_set = kX86_64;
      // TODO: x86_64 compilation support.
      runtime_->SetCompilerFilter(Runtime::kInterpretOnly);
#endif

      for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
        Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
        if (!runtime_->HasCalleeSaveMethod(type)) {
          runtime_->SetCalleeSaveMethod(
              runtime_->CreateCalleeSaveMethod(instruction_set, type), type);
        }
      }
      class_linker_->FixupDexCaches(runtime_->GetResolutionMethod());
      timer_.reset(new CumulativeLogger("Compilation times"));
      compiler_driver_.reset(new CompilerDriver(verification_results_.get(),
                                                method_inliner_map_.get(),
                                                compiler_backend, instruction_set,
                                                instruction_set_features,
                                                true, new CompilerDriver::DescriptorSet,
                                                2, true, true, timer_.get()));
    }
    // We typically don't generate an image in unit tests, disable this optimization by default.
    compiler_driver_->SetSupportBootImageFixup(false);

    // We're back in native, take the opportunity to initialize well known classes.
    WellKnownClasses::Init(Thread::Current()->GetJniEnv());
    // Create the heap thread pool so that the GC runs in parallel for tests. Normally, the thread
    // pool is created by the runtime.
    runtime_->GetHeap()->CreateThreadPool();
    runtime_->GetHeap()->VerifyHeap();  // Check for heap corruption before the test
  }

  virtual void TearDown() {
    const char* android_data = getenv("ANDROID_DATA");
    ASSERT_TRUE(android_data != NULL);
    DIR* dir = opendir(dalvik_cache_.c_str());
    ASSERT_TRUE(dir != NULL);
    dirent* e;
    while ((e = readdir(dir)) != NULL) {
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
    CHECK(sym != NULL);
    IcuCleanupFn icu_cleanup_fn = reinterpret_cast<IcuCleanupFn>(sym);
    (*icu_cleanup_fn)();

    compiler_driver_.reset();
    timer_.reset();
    callbacks_.Reset(nullptr, nullptr);
    method_inliner_map_.reset();
    verification_results_.reset();
    STLDeleteElements(&opened_dex_files_);

    Runtime::Current()->GetHeap()->VerifyHeap();  // Check for heap corruption after the test
  }

  std::string GetLibCoreDexFileName() {
    return GetDexFileName("core-libart");
  }

  std::string GetDexFileName(const std::string& jar_prefix) {
    if (IsHost()) {
      const char* host_dir = getenv("ANDROID_HOST_OUT");
      CHECK(host_dir != NULL);
      return StringPrintf("%s/framework/%s-hostdex.jar", host_dir, jar_prefix.c_str());
    }
    return StringPrintf("%s/framework/%s.jar", GetAndroidRoot(), jar_prefix.c_str());
  }

  std::string GetTestAndroidRoot() {
    if (IsHost()) {
      const char* host_dir = getenv("ANDROID_HOST_OUT");
      CHECK(host_dir != NULL);
      return host_dir;
    }
    return GetAndroidRoot();
  }

  const DexFile* OpenTestDexFile(const char* name) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(name != NULL);
    std::string filename;
    if (IsHost()) {
      filename += getenv("ANDROID_HOST_OUT");
      filename += "/framework/";
    } else {
      filename += "/data/nativetest/art/";
    }
    filename += "art-test-dex-";
    filename += name;
    filename += ".jar";
    std::string error_msg;
    const DexFile* dex_file = DexFile::Open(filename.c_str(), filename.c_str(), &error_msg);
    CHECK(dex_file != NULL) << "Failed to open '" << filename << "': " << error_msg;
    CHECK_EQ(PROT_READ, dex_file->GetPermissions());
    CHECK(dex_file->IsReadOnly());
    opened_dex_files_.push_back(dex_file);
    return dex_file;
  }

  jobject LoadDex(const char* dex_name) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const DexFile* dex_file = OpenTestDexFile(dex_name);
    CHECK(dex_file != NULL);
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

  void CompileClass(mirror::ClassLoader* class_loader, const char* class_name)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::string class_descriptor(DotToDescriptor(class_name));
    SirtRef<mirror::ClassLoader> loader(Thread::Current(), class_loader);
    mirror::Class* klass = class_linker_->FindClass(class_descriptor.c_str(), loader);
    CHECK(klass != NULL) << "Class not found " << class_name;
    for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
      CompileMethod(klass->GetDirectMethod(i));
    }
    for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
      CompileMethod(klass->GetVirtualMethod(i));
    }
  }

  void CompileMethod(mirror::ArtMethod* method) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK(method != NULL);
    TimingLogger timings("CommonTest::CompileMethod", false, false);
    timings.StartSplit("CompileOne");
    compiler_driver_->CompileOne(method, timings);
    MakeExecutable(method);
    timings.EndSplit();
  }

  void CompileDirectMethod(SirtRef<mirror::ClassLoader>& class_loader, const char* class_name,
                           const char* method_name, const char* signature)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::string class_descriptor(DotToDescriptor(class_name));
    mirror::Class* klass = class_linker_->FindClass(class_descriptor.c_str(), class_loader);
    CHECK(klass != NULL) << "Class not found " << class_name;
    mirror::ArtMethod* method = klass->FindDirectMethod(method_name, signature);
    CHECK(method != NULL) << "Direct method not found: "
                          << class_name << "." << method_name << signature;
    CompileMethod(method);
  }

  void CompileVirtualMethod(SirtRef<mirror::ClassLoader>& class_loader, const char* class_name,
                            const char* method_name, const char* signature)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::string class_descriptor(DotToDescriptor(class_name));
    mirror::Class* klass = class_linker_->FindClass(class_descriptor.c_str(), class_loader);
    CHECK(klass != NULL) << "Class not found " << class_name;
    mirror::ArtMethod* method = klass->FindVirtualMethod(method_name, signature);
    CHECK(method != NULL) << "Virtual method not found: "
                          << class_name << "." << method_name << signature;
    CompileMethod(method);
  }

  void ReserveImageSpace() {
    // Reserve where the image will be loaded up front so that other parts of test set up don't
    // accidentally end up colliding with the fixed memory address when we need to load the image.
    std::string error_msg;
    image_reservation_.reset(MemMap::MapAnonymous("image reservation",
                                                  reinterpret_cast<byte*>(ART_BASE_ADDRESS),
                                                  (size_t)100 * 1024 * 1024,  // 100MB
                                                  PROT_NONE,
                                                  false /* no need for 4gb flag with fixed mmap*/,
                                                  &error_msg));
    CHECK(image_reservation_.get() != nullptr) << error_msg;
  }

  void UnreserveImageSpace() {
    image_reservation_.reset();
  }

  class TestCompilerCallbacks : public CompilerCallbacks {
   public:
    TestCompilerCallbacks() : verification_results_(nullptr), method_inliner_map_(nullptr) {}

    void Reset(VerificationResults* verification_results,
               DexFileToMethodInlinerMap* method_inliner_map) {
        verification_results_ = verification_results;
        method_inliner_map_ = method_inliner_map;
    }

    virtual bool MethodVerified(verifier::MethodVerifier* verifier)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      CHECK(verification_results_);
      bool result = verification_results_->ProcessVerifiedMethod(verifier);
      if (result && method_inliner_map_ != nullptr) {
        MethodReference ref = verifier->GetMethodReference();
        method_inliner_map_->GetMethodInliner(ref.dex_file)
            ->AnalyseMethodCode(verifier);
      }
      return result;
    }
    virtual void ClassRejected(ClassReference ref) {
      verification_results_->AddRejectedClass(ref);
    }

   private:
    VerificationResults* verification_results_;
    DexFileToMethodInlinerMap* method_inliner_map_;
  };

  std::string android_data_;
  std::string dalvik_cache_;
  const DexFile* java_lang_dex_file_;  // owned by runtime_
  std::vector<const DexFile*> boot_class_path_;
  UniquePtr<Runtime> runtime_;
  // Owned by the runtime
  ClassLinker* class_linker_;
  UniquePtr<VerificationResults> verification_results_;
  UniquePtr<DexFileToMethodInlinerMap> method_inliner_map_;
  TestCompilerCallbacks callbacks_;
  UniquePtr<CompilerDriver> compiler_driver_;
  UniquePtr<CumulativeLogger> timer_;

 private:
  std::vector<const DexFile*> opened_dex_files_;
  UniquePtr<MemMap> image_reservation_;
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
    vm_->check_jni_abort_hook = NULL;
    vm_->check_jni_abort_hook_data = NULL;
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

}  // namespace art

namespace std {

// TODO: isn't gtest supposed to be able to print STL types for itself?
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& rhs) {
  os << ::art::ToString(rhs);
  return os;
}

}  // namespace std

#endif  // ART_RUNTIME_COMMON_TEST_H_
