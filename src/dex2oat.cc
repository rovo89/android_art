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

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "class_linker.h"
#include "class_loader.h"
#include "compiler.h"
#include "file.h"
#include "image_writer.h"
#include "leb128.h"
#include "oat_writer.h"
#include "object_utils.h"
#include "os.h"
#include "runtime.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "sirt_ref.h"
#include "stl_util.h"
#include "stringpiece.h"
#include "timing_logger.h"
#include "well_known_classes.h"
#include "zip_archive.h"

namespace art {

static void UsageErrorV(const char* fmt, va_list ap) {
  std::string error;
  StringAppendV(&error, fmt, ap);
  LOG(ERROR) << error;
}

static void UsageError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);
}

static void Usage(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);

  UsageError("Usage: dex2oat [options]...");
  UsageError("");
  UsageError("  --dex-file=<dex-file>: specifies a .dex file to compile.");
  UsageError("      Example: --dex-file=/system/framework/core.jar");
  UsageError("");
  UsageError("  --zip-fd=<file-descriptor>: specifies a file descriptor of a zip file");
  UsageError("      containing a classes.dex file to compile.");
  UsageError("      Example: --zip-fd=5");
  UsageError("");
  UsageError("  --zip-location=<zip-location>: specifies a symbolic name for the file corresponding");
  UsageError("      to the file descriptor specified by --zip-fd.");
  UsageError("      Example: --zip-location=/system/app/Calculator.apk");
  UsageError("");
  UsageError("  --oat-file=<file.oat>: specifies the required oat filename.");
  UsageError("      Example: --oat-file=/system/framework/boot.oat");
  UsageError("");
  UsageError("  --oat-location=<oat-name>: specifies a symbolic name for the file corresponding");
  UsageError("      to the file descriptor specified by --oat-fd.");
  UsageError("      Example: --oat-location=/data/art-cache/system@app@Calculator.apk.oat");
  UsageError("");
  UsageError("  --bitcode=<file.bc>: specifies the optional bitcode filename.");
  UsageError("      Example: --bitcode=/system/framework/boot.bc");
  UsageError("");
  UsageError("  --image=<file.art>: specifies the output image filename.");
  UsageError("      Example: --image=/system/framework/boot.art");
  UsageError("");
  UsageError("  --image-classes=<classname-file>: specifies classes to include in an image.");
  UsageError("      Example: --image=frameworks/base/preloaded-classes");
  UsageError("");
  UsageError("  --base=<hex-address>: specifies the base address when creating a boot image.");
  UsageError("      Example: --base=0x50000000");
  UsageError("");
  UsageError("  --boot-image=<file.art>: provide the image file for the boot class path.");
  UsageError("      Example: --boot-image=/system/framework/boot.art");
  UsageError("      Default: <host-prefix>/system/framework/boot.art");
  UsageError("");
  UsageError("  --host-prefix may be used to translate host paths to target paths during");
  UsageError("      cross compilation.");
  UsageError("      Example: --host-prefix=out/target/product/crespo");
  UsageError("      Default: $ANDROID_PRODUCT_OUT");
  UsageError("");
  UsageError("  --instruction-set=(arm|mips|x86): compile for a particular instruction");
  UsageError("      set.");
  UsageError("      Example: --instruction-set=x86");
  UsageError("      Default: arm");
  UsageError("");
  UsageError("  --compiler-backend=(Quick|QuickGBC|Portable): select compiler backend");
  UsageError("      set.");
  UsageError("      Example: --instruction-set=Portable");
  UsageError("      Default: Quick");
  UsageError("  --runtime-arg <argument>: used to specify various arguments for the runtime,");
  UsageError("      such as initial heap size, maximum heap size, and verbose output.");
  UsageError("      Use a separate --runtime-arg switch for each argument.");
  UsageError("      Example: --runtime-arg -Xms256m");
  UsageError("");
  std::cerr << "See log for usage error information\n";
  exit(EXIT_FAILURE);
}

class Dex2Oat {
 public:
  static bool Create(Dex2Oat** p_dex2oat, Runtime::Options& options, CompilerBackend compiler_backend,
                     InstructionSet instruction_set, size_t thread_count, bool support_debugging)
      SHARED_TRYLOCK_FUNCTION(true, Locks::mutator_lock_) {
    if (!CreateRuntime(options, instruction_set)) {
      *p_dex2oat = NULL;
      return false;
    }
    *p_dex2oat = new Dex2Oat(Runtime::Current(), compiler_backend, instruction_set, thread_count,
                             support_debugging);
    return true;
  }

  ~Dex2Oat() {
    delete runtime_;
    LOG(INFO) << "dex2oat took " << PrettyDuration(NanoTime() - start_ns_) << " (threads: " << thread_count_ << ")";
  }

  // Make a list of descriptors for classes to include in the image
  const std::set<std::string>* GetImageClassDescriptors(const char* image_classes_filename)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    UniquePtr<std::ifstream> image_classes_file(new std::ifstream(image_classes_filename, std::ifstream::in));
    if (image_classes_file.get() == NULL) {
      LOG(ERROR) << "Failed to open image classes file " << image_classes_filename;
      return NULL;
    }

    // Load all the classes specified in the file
    ClassLinker* class_linker = runtime_->GetClassLinker();
    Thread* self = Thread::Current();
    while (image_classes_file->good()) {
      std::string dot;
      std::getline(*image_classes_file.get(), dot);
      if (StartsWith(dot, "#") || dot.empty()) {
        continue;
      }
      std::string descriptor(DotToDescriptor(dot.c_str()));
      SirtRef<Class> klass(self, class_linker->FindSystemClass(descriptor.c_str()));
      if (klass.get() == NULL) {
        LOG(WARNING) << "Failed to find class " << descriptor;
        Thread::Current()->ClearException();
      }
    }
    image_classes_file->close();

    // Resolve exception classes referenced by the loaded classes. The catch logic assumes
    // exceptions are resolved by the verifier when there is a catch block in an interested method.
    // Do this here so that exception classes appear to have been specified image classes.
    std::set<std::pair<uint16_t, const DexFile*> > unresolved_exception_types;
    SirtRef<Class> java_lang_Throwable(self,
                                       class_linker->FindSystemClass("Ljava/lang/Throwable;"));
    do {
      unresolved_exception_types.clear();
      class_linker->VisitClasses(ResolveCatchBlockExceptionsClassVisitor,
                                 &unresolved_exception_types);
      typedef std::set<std::pair<uint16_t, const DexFile*> >::const_iterator It;  // TODO: C++0x auto
      for (It it = unresolved_exception_types.begin(),
           end = unresolved_exception_types.end();
           it != end; ++it) {
        uint16_t exception_type_idx = it->first;
        const DexFile* dex_file = it->second;
        DexCache* dex_cache = class_linker->FindDexCache(*dex_file);
        ClassLoader* class_loader = NULL;
        SirtRef<Class> klass(self, class_linker->ResolveType(*dex_file, exception_type_idx,
                                                             dex_cache, class_loader));
        if (klass.get() == NULL) {
          const DexFile::TypeId& type_id = dex_file->GetTypeId(exception_type_idx);
          const char* descriptor = dex_file->GetTypeDescriptor(type_id);
          LOG(FATAL) << "Failed to resolve class " << descriptor;
        }
        DCHECK(java_lang_Throwable->IsAssignableFrom(klass.get()));
      }
      // Resolving exceptions may load classes that reference more exceptions, iterate until no
      // more are found
    } while (!unresolved_exception_types.empty());

    // We walk the roots looking for classes so that we'll pick up the
    // above classes plus any classes them depend on such super
    // classes, interfaces, and the required ClassLinker roots.
    UniquePtr<std::set<std::string> > image_classes(new std::set<std::string>());
    class_linker->VisitClasses(RecordImageClassesVisitor, image_classes.get());
    CHECK_NE(image_classes->size(), 0U);
    return image_classes.release();
  }

  const Compiler* CreateOatFile(const std::string& boot_image_option,
                                const std::string* host_prefix,
                                const std::vector<const DexFile*>& dex_files,
                                File* oat_file,
                                const std::string& bitcode_filename,
                                bool image,
                                const std::set<std::string>* image_classes,
                                bool dump_stats,
                                bool dump_timings)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // SirtRef and ClassLoader creation needs to come after Runtime::Create
    jobject class_loader = NULL;
    if (!boot_image_option.empty()) {
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      std::vector<const DexFile*> class_path_files(dex_files);
      OpenClassPathFiles(runtime_->GetClassPathString(), class_path_files);
      for (size_t i = 0; i < class_path_files.size(); i++) {
        class_linker->RegisterDexFile(*class_path_files[i]);
      }
      ScopedObjectAccessUnchecked soa(Thread::Current());
      soa.Env()->AllocObject(WellKnownClasses::dalvik_system_PathClassLoader);
      ScopedLocalRef<jobject> class_loader_local(soa.Env(),
          soa.Env()->AllocObject(WellKnownClasses::dalvik_system_PathClassLoader));
      class_loader = soa.Env()->NewGlobalRef(class_loader_local.get());
      Runtime::Current()->SetCompileTimeClassPath(class_loader, class_path_files);
    }

    UniquePtr<Compiler> compiler(new Compiler(compiler_backend_,
                                              instruction_set_,
                                              image,
                                              thread_count_,
                                              support_debugging_,
                                              image_classes,
                                              dump_stats,
                                              dump_timings));

    if ((compiler_backend_ == kPortable) || (compiler_backend_ == kIceland)) {
      compiler->SetBitcodeFileName(bitcode_filename);
    }

    Thread::Current()->TransitionFromRunnableToSuspended(kNative);

    compiler->CompileAll(class_loader, dex_files);

    Thread::Current()->TransitionFromSuspendedToRunnable();

    std::string image_file_location;
    uint32_t image_file_location_oat_checksum = 0;
    uint32_t image_file_location_oat_begin = 0;
    Heap* heap = Runtime::Current()->GetHeap();
    if (heap->GetSpaces().size() > 1) {
      ImageSpace* image_space = heap->GetImageSpace();
      image_file_location_oat_checksum = image_space->GetImageHeader().GetOatChecksum();
      image_file_location_oat_begin = reinterpret_cast<uint32_t>(image_space->GetImageHeader().GetOatBegin());
      image_file_location = image_space->GetImageFilename();
      if (host_prefix != NULL && StartsWith(image_file_location, host_prefix->c_str())) {
        image_file_location = image_file_location.substr(host_prefix->size());
      }
    }

    if (!OatWriter::Create(oat_file,
                           dex_files,
                           image_file_location_oat_checksum,
                           image_file_location_oat_begin,
                           image_file_location,
                           *compiler.get())) {
      LOG(ERROR) << "Failed to create oat file " << oat_file->name();
      return NULL;
    }
    return compiler.release();
  }

  bool CreateImageFile(const std::string& image_filename,
                       uintptr_t image_base,
                       const std::set<std::string>* image_classes,
                       const std::string& oat_filename,
                       const std::string& oat_location,
                       const Compiler& compiler)
      LOCKS_EXCLUDED(Locks::mutator_lock_) {
    ImageWriter image_writer(image_classes);
    if (!image_writer.Write(image_filename, image_base, oat_filename, oat_location, compiler)) {
      LOG(ERROR) << "Failed to create image file " << image_filename;
      return false;
    }
    return true;
  }

 private:
  explicit Dex2Oat(Runtime* runtime, CompilerBackend compiler_backend, InstructionSet instruction_set,
                   size_t thread_count, bool support_debugging)
      : compiler_backend_(compiler_backend),
        instruction_set_(instruction_set),
        runtime_(runtime),
        thread_count_(thread_count),
        support_debugging_(support_debugging),
        start_ns_(NanoTime()) {
  }

  static bool CreateRuntime(Runtime::Options& options, InstructionSet instruction_set)
      SHARED_TRYLOCK_FUNCTION(true, Locks::mutator_lock_) {
    if (!Runtime::Create(options, false)) {
      LOG(ERROR) << "Failed to create runtime";
      return false;
    }
    Runtime* runtime = Runtime::Current();
    // if we loaded an existing image, we will reuse values from the image roots.
    if (!runtime->HasJniDlsymLookupStub()) {
      runtime->SetJniDlsymLookupStub(Compiler::CreateJniDlsymLookupStub(instruction_set));
    }
    if (!runtime->HasAbstractMethodErrorStubArray()) {
      runtime->SetAbstractMethodErrorStubArray(Compiler::CreateAbstractMethodErrorStub(instruction_set));
    }
    for (int i = 0; i < Runtime::kLastTrampolineMethodType; i++) {
      Runtime::TrampolineType type = Runtime::TrampolineType(i);
      if (!runtime->HasResolutionStubArray(type)) {
        runtime->SetResolutionStubArray(Compiler::CreateResolutionStub(instruction_set, type), type);
      }
    }
    if (!runtime->HasResolutionMethod()) {
      runtime->SetResolutionMethod(runtime->CreateResolutionMethod());
    }
    for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
      Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
      if (!runtime->HasCalleeSaveMethod(type)) {
        runtime->SetCalleeSaveMethod(runtime->CreateCalleeSaveMethod(instruction_set, type), type);
      }
    }
    runtime->GetClassLinker()->FixupDexCaches(runtime->GetResolutionMethod());
    return true;
  }

  static void ResolveExceptionsForMethod(MethodHelper* mh,
                           std::set<std::pair<uint16_t, const DexFile*> >& exceptions_to_resolve)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const DexFile::CodeItem* code_item = mh->GetCodeItem();
    if (code_item == NULL) {
      return;  // native or abstract method
    }
    if (code_item->tries_size_ == 0) {
      return;  // nothing to process
    }
    const byte* encoded_catch_handler_list = DexFile::GetCatchHandlerData(*code_item, 0);
    size_t num_encoded_catch_handlers = DecodeUnsignedLeb128(&encoded_catch_handler_list);
    for (size_t i = 0; i < num_encoded_catch_handlers; i++) {
      int32_t encoded_catch_handler_size = DecodeSignedLeb128(&encoded_catch_handler_list);
      bool has_catch_all = false;
      if (encoded_catch_handler_size <= 0) {
        encoded_catch_handler_size = -encoded_catch_handler_size;
        has_catch_all = true;
      }
      for (int32_t j = 0; j < encoded_catch_handler_size; j++) {
        uint16_t encoded_catch_handler_handlers_type_idx =
            DecodeUnsignedLeb128(&encoded_catch_handler_list);
        // Add to set of types to resolve if not already in the dex cache resolved types
        if (!mh->IsResolvedTypeIdx(encoded_catch_handler_handlers_type_idx)) {
          exceptions_to_resolve.insert(
              std::pair<uint16_t, const DexFile*>(encoded_catch_handler_handlers_type_idx,
                                                  &mh->GetDexFile()));
        }
        // ignore address associated with catch handler
        DecodeUnsignedLeb128(&encoded_catch_handler_list);
      }
      if (has_catch_all) {
        // ignore catch all address
        DecodeUnsignedLeb128(&encoded_catch_handler_list);
      }
    }
  }

  static bool ResolveCatchBlockExceptionsClassVisitor(Class* c, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::set<std::pair<uint16_t, const DexFile*> >* exceptions_to_resolve =
        reinterpret_cast<std::set<std::pair<uint16_t, const DexFile*> >*>(arg);
    MethodHelper mh;
    for (size_t i = 0; i < c->NumVirtualMethods(); ++i) {
      AbstractMethod* m = c->GetVirtualMethod(i);
      mh.ChangeMethod(m);
      ResolveExceptionsForMethod(&mh, *exceptions_to_resolve);
    }
    for (size_t i = 0; i < c->NumDirectMethods(); ++i) {
      AbstractMethod* m = c->GetDirectMethod(i);
      mh.ChangeMethod(m);
      ResolveExceptionsForMethod(&mh, *exceptions_to_resolve);
    }
    return true;
  }

  static bool RecordImageClassesVisitor(Class* klass, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::set<std::string>* image_classes = reinterpret_cast<std::set<std::string>*>(arg);
    if (klass->IsArrayClass() || klass->IsPrimitive()) {
      return true;
    }
    image_classes->insert(ClassHelper(klass).GetDescriptor());
    return true;
  }

  // Appends to dex_files any elements of class_path that it doesn't already
  // contain. This will open those dex files as necessary.
  static void OpenClassPathFiles(const std::string& class_path, std::vector<const DexFile*>& dex_files) {
    std::vector<std::string> parsed;
    Split(class_path, ':', parsed);
    for (size_t i = 0; i < parsed.size(); ++i) {
      if (DexFilesContains(dex_files, parsed[i])) {
        continue;
      }
      const DexFile* dex_file = DexFile::Open(parsed[i], parsed[i]);
      if (dex_file == NULL) {
        LOG(WARNING) << "Failed to open dex file " << parsed[i];
      } else {
        dex_files.push_back(dex_file);
      }
    }
  }

  // Returns true if dex_files has a dex with the named location.
  static bool DexFilesContains(const std::vector<const DexFile*>& dex_files, const std::string& location) {
    for (size_t i = 0; i < dex_files.size(); ++i) {
      if (dex_files[i]->GetLocation() == location) {
        return true;
      }
    }
    return false;
  }

  const CompilerBackend compiler_backend_;

  const InstructionSet instruction_set_;

  Runtime* runtime_;
  size_t thread_count_;
  bool support_debugging_;
  uint64_t start_ns_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Dex2Oat);
};

static bool ParseInt(const char* in, int* out) {
  char* end;
  int result = strtol(in, &end, 10);
  if (in == end || *end != '\0') {
    return false;
  }
  *out = result;
  return true;
}

static size_t OpenDexFiles(const std::vector<const char*>& dex_filenames,
                           const std::vector<const char*>& dex_locations,
                           std::vector<const DexFile*>& dex_files) {
  size_t failure_count = 0;
  for (size_t i = 0; i < dex_filenames.size(); i++) {
    const char* dex_filename = dex_filenames[i];
    const char* dex_location = dex_locations[i];
    const DexFile* dex_file = DexFile::Open(dex_filename, dex_location);
    if (dex_file == NULL) {
      LOG(WARNING) << "could not open .dex from file " << dex_filename;
      ++failure_count;
    } else {
      dex_files.push_back(dex_file);
    }
  }
  return failure_count;
}

// The primary goal of the watchdog is to prevent stuck build servers
// during development when fatal aborts lead to a cascade of failures
// that result in a deadlock.
class WatchDog {

// WatchDog defines its own CHECK_PTHREAD_CALL to avoid using Log which uses locks
#undef CHECK_PTHREAD_CALL
#define CHECK_WATCH_DOG_PTHREAD_CALL(call, args, what) \
  do { \
    int rc = call args; \
    if (rc != 0) { \
      errno = rc; \
      std::string message(# call); \
      message += " failed for "; \
      message += reason; \
      Die(message); \
    } \
  } while (false)

 public:
  WatchDog() {
    if (!kIsWatchDogEnabled) {
      return;
    }
    shutting_down_ = false;
    const char* reason = "dex2oat watch dog thread startup";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_init, (&mutex_, NULL), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_init, (&cond_, NULL), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_attr_init, (&attr_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_create, (&pthread_, &attr_, &CallBack, this), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_attr_destroy, (&attr_), reason);
  }
  ~WatchDog() {
    if (!kIsWatchDogEnabled) {
      return;
    }
    const char* reason = "dex2oat watch dog thread shutdown";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_lock, (&mutex_), reason);
    shutting_down_ = true;
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_signal, (&cond_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_unlock, (&mutex_), reason);

    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_join, (pthread_, NULL), reason);

    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_destroy, (&cond_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_destroy, (&mutex_), reason);
  }

 private:
  static void* CallBack(void* arg) {
    WatchDog* self = reinterpret_cast<WatchDog*>(arg);
    self->Wait();
    return NULL;
  }

  static void Die(const std::string& message) {
    // TODO: Switch to LOG(FATAL) when we can guarantee it won't prevent shutdown in error cases
    fprintf(stderr, "%s\n", message.c_str());
    exit(1);
  }

  void Wait() {
    int64_t ms = kWatchDogTimeoutSeconds * 1000;
    int32_t ns = 0;
    timespec ts;
    InitTimeSpec(true, CLOCK_REALTIME, ms, ns, &ts);
    const char* reason = "dex2oat watch dog thread waiting";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_lock, (&mutex_), reason);
    while (!shutting_down_) {
      int rc = TEMP_FAILURE_RETRY(pthread_cond_timedwait(&cond_, &mutex_, &ts));
      if (rc == ETIMEDOUT) {
        std::string message(StringPrintf("dex2oat did not finish after %d seconds",
                                         kWatchDogTimeoutSeconds));
        Die(message.c_str());
      }
      if (rc != 0) {
        std::string message(StringPrintf("pthread_cond_timedwait failed: %s",
                                         strerror(errno)));
        Die(message.c_str());
      }
    }
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_unlock, (&mutex_), reason);
  }

  static const bool kIsWatchDogEnabled = !kIsTargetBuild;
#ifdef ART_USE_LLVM_COMPILER
  static const unsigned int kWatchDogTimeoutSeconds = 20 * 60; // 15 minutes + buffer
#else
  static const unsigned int kWatchDogTimeoutSeconds = 2 * 60;  // 1 minute + buffer
#endif

  bool shutting_down_;
  // TODO: Switch to Mutex when we can guarantee it won't prevent shutdown in error cases
  pthread_mutex_t mutex_;
  pthread_cond_t cond_;
  pthread_attr_t attr_;
  pthread_t pthread_;
};

static int dex2oat(int argc, char** argv) {
  WatchDog watch_dog;

  InitLogging(argv);

  // Skip over argv[0].
  argv++;
  argc--;

  if (argc == 0) {
    Usage("no arguments specified");
  }

  std::vector<const char*> dex_filenames;
  std::vector<const char*> dex_locations;
  int zip_fd = -1;
  std::string zip_location;
  std::string oat_filename;
  std::string oat_location;
  int oat_fd = -1;
  std::string bitcode_filename;
  const char* image_classes_filename = NULL;
  std::string image_filename;
  std::string boot_image_filename;
  uintptr_t image_base = 0;
  UniquePtr<std::string> host_prefix;
  std::vector<const char*> runtime_args;
  int thread_count = sysconf(_SC_NPROCESSORS_CONF);
  bool support_debugging = false;
#if defined(ART_USE_PORTABLE_COMPILER)
  CompilerBackend compiler_backend = kPortable;
#elif defined(ART_USE_LLVM_COMPILER)
  CompilerBackend compiler_backend = kIceland;
#else
  CompilerBackend compiler_backend = kQuick;
#endif
#if defined(__arm__)
  InstructionSet instruction_set = kThumb2;
#elif defined(__i386__)
  InstructionSet instruction_set = kX86;
#elif defined(__mips__)
  InstructionSet instruction_set = kMips;
#else
#error "Unsupported architecture"
#endif
  bool dump_stats = kIsDebugBuild;
  bool dump_timings = kIsDebugBuild;

  for (int i = 0; i < argc; i++) {
    const StringPiece option(argv[i]);
    bool log_options = false;
    if (log_options) {
      LOG(INFO) << "dex2oat: option[" << i << "]=" << argv[i];
    }
    if (option.starts_with("--dex-file=")) {
      dex_filenames.push_back(option.substr(strlen("--dex-file=")).data());
    } else if (option.starts_with("--dex-location=")) {
      dex_locations.push_back(option.substr(strlen("--dex-location=")).data());
    } else if (option.starts_with("--zip-fd=")) {
      const char* zip_fd_str = option.substr(strlen("--zip-fd=")).data();
      if (!ParseInt(zip_fd_str, &zip_fd)) {
        Usage("could not parse --zip-fd argument '%s' as an integer", zip_fd_str);
      }
    } else if (option.starts_with("--zip-location=")) {
      zip_location = option.substr(strlen("--zip-location=")).data();
    } else if (option.starts_with("--oat-file=")) {
      oat_filename = option.substr(strlen("--oat-file=")).data();
    } else if (option.starts_with("--oat-fd=")) {
      const char* oat_fd_str = option.substr(strlen("--oat-fd=")).data();
      if (!ParseInt(oat_fd_str, &oat_fd)) {
        Usage("could not parse --oat-fd argument '%s' as an integer", oat_fd_str);
      }
    } else if (option.starts_with("-g")) {
      support_debugging = true;
    } else if (option.starts_with("-j")) {
      const char* thread_count_str = option.substr(strlen("-j")).data();
      if (!ParseInt(thread_count_str, &thread_count)) {
        Usage("could not parse -j argument '%s' as an integer", thread_count_str);
      }
    } else if (option.starts_with("--oat-location=")) {
      oat_location = option.substr(strlen("--oat-location=")).data();
    } else if (option.starts_with("--bitcode=")) {
      bitcode_filename = option.substr(strlen("--bitcode=")).data();
    } else if (option.starts_with("--image=")) {
      image_filename = option.substr(strlen("--image=")).data();
    } else if (option.starts_with("--image-classes=")) {
      image_classes_filename = option.substr(strlen("--image-classes=")).data();
    } else if (option.starts_with("--base=")) {
      const char* image_base_str = option.substr(strlen("--base=")).data();
      char* end;
      image_base = strtoul(image_base_str, &end, 16);
      if (end == image_base_str || *end != '\0') {
        Usage("Failed to parse hexadecimal value for option %s", option.data());
      }
    } else if (option.starts_with("--boot-image=")) {
      boot_image_filename = option.substr(strlen("--boot-image=")).data();
    } else if (option.starts_with("--host-prefix=")) {
      host_prefix.reset(new std::string(option.substr(strlen("--host-prefix=")).data()));
    } else if (option.starts_with("--instruction-set=")) {
      StringPiece instruction_set_str = option.substr(strlen("--instruction-set=")).data();
      if (instruction_set_str == "arm") {
        instruction_set = kThumb2;
      } else if (instruction_set_str == "mips") {
        instruction_set = kMips;
      } else if (instruction_set_str == "x86") {
        instruction_set = kX86;
      }
    } else if (option.starts_with("--compiler-backend=")) {
      StringPiece backend_str = option.substr(strlen("--compiler-backend=")).data();
      if (backend_str == "Quick") {
        compiler_backend = kQuick;
      } else if (backend_str == "QuickGBC") {
        compiler_backend = kQuickGBC;
      } else if (backend_str == "Iceland") {
        // TODO: remove this when Portable/Iceland merge complete
        compiler_backend = kIceland;
      } else if (backend_str == "Portable") {
        compiler_backend = kPortable;
      }
    } else if (option == "--runtime-arg") {
      if (++i >= argc) {
        Usage("Missing required argument for --runtime-arg");
      }
      if (log_options) {
        LOG(INFO) << "dex2oat: option[" << i << "]=" << argv[i];
      }
      runtime_args.push_back(argv[i]);
    } else {
      Usage("unknown argument %s", option.data());
    }
  }

  if (oat_filename.empty() && oat_fd == -1) {
    Usage("Output must be supplied with either --oat-file or --oat-fd");
  }

  if (!oat_filename.empty() && oat_fd != -1) {
    Usage("--oat-file should not be used with --oat-fd");
  }

  if (!oat_filename.empty() && oat_fd != -1) {
    Usage("--oat-file should not be used with --oat-fd");
  }

  if (oat_fd != -1 && !image_filename.empty()) {
    Usage("--oat-fd should not be used with --image");
  }

  if (host_prefix.get() == NULL) {
    const char* android_product_out = getenv("ANDROID_PRODUCT_OUT");
    if (android_product_out != NULL) {
        host_prefix.reset(new std::string(android_product_out));
    }
  }

  bool image = (!image_filename.empty());
  if (!image && boot_image_filename.empty()) {
    if (host_prefix.get() == NULL) {
      boot_image_filename += GetAndroidRoot();
    } else {
      boot_image_filename += *host_prefix.get();
      boot_image_filename += "/system";
    }
    boot_image_filename += "/framework/boot.art";
  }
  std::string boot_image_option;
  if (!boot_image_filename.empty()) {
    boot_image_option += "-Ximage:";
    boot_image_option += boot_image_filename;
  }

  if (image_classes_filename != NULL && !image) {
    Usage("--image-classes should only be used with --image");
  }

  if (image_classes_filename != NULL && !boot_image_option.empty()) {
    Usage("--image-classes should not be used with --boot-image");
  }

  if (dex_filenames.empty() && zip_fd == -1) {
    Usage("Input must be supplied with either --dex-file or --zip-fd");
  }

  if (!dex_filenames.empty() && zip_fd != -1) {
    Usage("--dex-file should not be used with --zip-fd");
  }

  if (!dex_filenames.empty() && !zip_location.empty()) {
    Usage("--dex-file should not be used with --zip-location");
  }

  if (dex_locations.empty()) {
    for (size_t i = 0; i < dex_filenames.size(); i++) {
      dex_locations.push_back(dex_filenames[i]);
    }
  } else if (dex_locations.size() != dex_filenames.size()) {
    Usage("--dex-location arguments do not match --dex-file arguments");
  }

  if (zip_fd != -1 && zip_location.empty()) {
    Usage("--zip-location should be supplied with --zip-fd");
  }

  if (boot_image_option.empty()) {
    if (image_base == 0) {
      Usage("non-zero --base not specified");
    }
  }

  // Check early that the result of compilation can be written
  UniquePtr<File> oat_file;
  bool create_file = !oat_filename.empty();  // as opposed to using open file descriptor
  if (create_file) {
    oat_file.reset(OS::OpenFile(oat_filename.c_str(), true));
    if (oat_location.empty()) {
      oat_location = oat_filename;
    }
  } else {
    oat_file.reset(OS::FileFromFd(oat_location.c_str(), oat_fd));
  }
  if (oat_file.get() == NULL) {
    PLOG(ERROR) << "Failed to create oat file: " << oat_location;
    return EXIT_FAILURE;
  }
  if (create_file && fchmod(oat_file->Fd(), 0644) != 0) {
    PLOG(ERROR) << "Failed to make oat file world readable: " << oat_location;
    return EXIT_FAILURE;
  }

  LOG(INFO) << "dex2oat: " << oat_location;

  Runtime::Options options;
  options.push_back(std::make_pair("compiler", reinterpret_cast<void*>(NULL)));
  std::vector<const DexFile*> boot_class_path;
  if (boot_image_option.empty()) {
    size_t failure_count = OpenDexFiles(dex_filenames, dex_locations, boot_class_path);
    if (failure_count > 0) {
      LOG(ERROR) << "Failed to open some dex files: " << failure_count;
      return EXIT_FAILURE;
    }
    options.push_back(std::make_pair("bootclasspath", &boot_class_path));
  } else {
    options.push_back(std::make_pair(boot_image_option.c_str(), reinterpret_cast<void*>(NULL)));
  }
  if (host_prefix.get() != NULL) {
    options.push_back(std::make_pair("host-prefix", host_prefix->c_str()));
  }
  for (size_t i = 0; i < runtime_args.size(); i++) {
    options.push_back(std::make_pair(runtime_args[i], reinterpret_cast<void*>(NULL)));
  }

  Dex2Oat* p_dex2oat;
  if (!Dex2Oat::Create(&p_dex2oat, options, compiler_backend, instruction_set, thread_count, support_debugging)) {
    LOG(ERROR) << "Failed to create dex2oat";
    return EXIT_FAILURE;
  }
  UniquePtr<Dex2Oat> dex2oat(p_dex2oat);
  // Runtime::Create acquired the mutator_lock_ that is normally given away when we Runtime::Start,
  // give it away now and then switch to a more managable ScopedObjectAccess.
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);
  // Whilst we're in native take the opportunity to initialize well known classes.
  WellKnownClasses::InitClasses(Thread::Current()->GetJniEnv());
  ScopedObjectAccess soa(Thread::Current());

  // If --image-classes was specified, calculate the full list of classes to include in the image
  UniquePtr<const std::set<std::string> > image_classes(NULL);
  if (image_classes_filename != NULL) {
    image_classes.reset(dex2oat->GetImageClassDescriptors(image_classes_filename));
    if (image_classes.get() == NULL) {
      LOG(ERROR) << "Failed to create list of image classes from " << image_classes_filename;
      return EXIT_FAILURE;
    }
  }

  std::vector<const DexFile*> dex_files;
  if (boot_image_option.empty()) {
    dex_files = Runtime::Current()->GetClassLinker()->GetBootClassPath();
  } else {
    if (dex_filenames.empty()) {
      UniquePtr<ZipArchive> zip_archive(ZipArchive::OpenFromFd(zip_fd));
      if (zip_archive.get() == NULL) {
        LOG(ERROR) << "Failed to zip from file descriptor for " << zip_location;
        return EXIT_FAILURE;
      }
      const DexFile* dex_file = DexFile::Open(*zip_archive.get(), zip_location);
      if (dex_file == NULL) {
        LOG(ERROR) << "Failed to open dex from file descriptor for zip file: " << zip_location;
        return EXIT_FAILURE;
      }
      dex_files.push_back(dex_file);
    } else {
      size_t failure_count = OpenDexFiles(dex_filenames, dex_locations, dex_files);
      if (failure_count > 0) {
        LOG(ERROR) << "Failed to open some dex files: " << failure_count;
        return EXIT_FAILURE;
      }
    }
  }

  UniquePtr<const Compiler> compiler(dex2oat->CreateOatFile(boot_image_option,
                                                            host_prefix.get(),
                                                            dex_files,
                                                            oat_file.get(),
                                                            bitcode_filename,
                                                            image,
                                                            image_classes.get(),
                                                            dump_stats,
                                                            dump_timings));

  if (compiler.get() == NULL) {
    LOG(ERROR) << "Failed to create oat file: " << oat_location;
    return EXIT_FAILURE;
  }

  if (!image) {
    LOG(INFO) << "Oat file written successfully: " << oat_location;
    return EXIT_SUCCESS;
  }

  Thread::Current()->TransitionFromRunnableToSuspended(kNative);
  bool image_creation_success = dex2oat->CreateImageFile(image_filename,
                                                         image_base,
                                                         image_classes.get(),
                                                         oat_filename,
                                                         oat_location,
                                                         *compiler.get());
  Thread::Current()->TransitionFromSuspendedToRunnable();
  if (!image_creation_success) {
    return EXIT_FAILURE;
  }

  // We wrote the oat file successfully, and want to keep it.
  LOG(INFO) << "Oat file written successfully: " << oat_filename;
  LOG(INFO) << "Image written successfully: " << image_filename;
  return EXIT_SUCCESS;
}

} // namespace art

int main(int argc, char** argv) {
  return art::dex2oat(argc, argv);
}
