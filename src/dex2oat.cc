// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "class_linker.h"
#include "class_loader.h"
#include "compiler.h"
#include "file.h"
#include "image_writer.h"
#include "oat_writer.h"
#include "object_utils.h"
#include "os.h"
#include "runtime.h"
#include "stringpiece.h"

namespace art {

static void usage() {
  fprintf(stderr,
          "Usage: dex2oat [options]...\n"
          "\n");
  fprintf(stderr,
          "  --dex-file=<dex-file>: specifies a .dex file to compile. At least one .dex\n"
          "      file must be specified. \n"
          "      Example: --dex-file=/system/framework/core.jar\n"
          "\n");
  fprintf(stderr,
          "  --oat=<file.oat>: specifies the required oat filename.\n"
          "      Example: --oat=/data/art-cache/boot.oat\n"
          "\n");
  fprintf(stderr,
          "  --image=<file.art>: specifies the output image filename.\n"
          "      Example: --image=/data/art-cache/boot.art\n"
          "\n");
  fprintf(stderr,
          "  --image-classes=<classname-file>: specifies classes to include in an image.\n"
          "      Example: --image=frameworks/base/preloaded-classes\n"
          "\n");
  fprintf(stderr,
          "  --base=<hex-address>: specifies the base address when creating a boot image.\n"
          "      Example: --base=0x50000000\n"
          "\n");
  fprintf(stderr,
          "  --boot-image=<file.art>: provide the image file for the boot class path.\n"
          "      Example: --boot-image=/data/art-cache/boot.art\n"
          "\n");
  fprintf(stderr,
          "  --host-prefix may be used to translate host paths to target paths during\n"
          "      cross compilation.\n"
          "      Example: --host-prefix=out/target/product/crespo\n"
          "\n");
  fprintf(stderr,
          "  --runtime-arg <argument>: used to specify various arguments for the runtime,\n"
          "      such as initial heap size, maximum heap size, and verbose output.\n"
          "      Use a separate --runtime-arg switch for each argument.\n"
          "      Example: --runtime-arg -Xms256m\n"
          "\n");
  exit(EXIT_FAILURE);
}

class FileJanitor {
public:
  FileJanitor(const std::string& filename, int fd)
      : filename_(filename), fd_(fd), do_unlink_(true) {
  }

  void KeepFile() {
    do_unlink_ = false;
  }

  ~FileJanitor() {
    if (fd_ != -1) {
      int rc = TEMP_FAILURE_RETRY(flock(fd_, LOCK_UN));
      if (rc == -1) {
        PLOG(ERROR) << "Failed to unlock " << filename_;
      }
    }
    if (do_unlink_) {
      int rc = TEMP_FAILURE_RETRY(unlink(filename_.c_str()));
      if (rc == -1) {
        PLOG(ERROR) << "Failed to unlink " << filename_;
      }
    }
  }

private:
  std::string filename_;
  int fd_;
  bool do_unlink_;
};

class Dex2Oat {
 public:

  static Dex2Oat* Create(Runtime::Options& options) {
    UniquePtr<Runtime> runtime(CreateRuntime(options));
    if (runtime.get() == NULL) {
      return NULL;
    }
    return new Dex2Oat(runtime.release());
  }

  ~Dex2Oat() {
    delete runtime_;
  }

  // Make a list of descriptors for classes to include in the image
  const std::set<std::string>* GetImageClassDescriptors(const char* image_classes_filename) {
    UniquePtr<std::ifstream> image_classes_file(new std::ifstream(image_classes_filename, std::ifstream::in));
    if (image_classes_file.get() == NULL) {
      LOG(ERROR) << "Failed to open image classes file " << image_classes_filename;
      return NULL;
    }

    // Load all the classes specifed in the file
    ClassLinker* class_linker = runtime_->GetClassLinker();
    while (image_classes_file->good()) {
      std::string dot;
      std::getline(*image_classes_file.get(), dot);
      if (StringPiece(dot).starts_with("#") || dot.empty()) {
        continue;
      }
      std::string descriptor = DotToDescriptor(dot.c_str());
      SirtRef<Class> klass(class_linker->FindSystemClass(descriptor));
      if (klass.get() == NULL) {
        LOG(WARNING) << "Failed to find class " << descriptor;
        Thread::Current()->ClearException();
      }
    }
    image_classes_file->close();

    // We walk the roots looking for classes so that we'll pick up the
    // above classes plus any classes them depend on such super
    // classes, interfaces, and the required ClassLinker roots.
    UniquePtr<std::set<std::string> > image_classes(new std::set<std::string>());
    class_linker->VisitClasses(ClassVisitor, image_classes.get());
    CHECK_NE(image_classes->size(), 0U);
    return image_classes.release();
  }

  bool CreateOatFile(const std::string& boot_image_option,
                     const std::vector<const char*>& dex_filenames,
                     const std::string& host_prefix,
                     File* oat_file,
                     bool image,
                     const std::set<std::string>* image_classes) {
    // SirtRef and ClassLoader creation needs to come after Runtime::Create
    UniquePtr<SirtRef<ClassLoader> > class_loader(new SirtRef<ClassLoader>(NULL));
    if (class_loader.get() == NULL) {
      LOG(ERROR) << "Failed to create SirtRef for class loader";
      return false;
    }

    std::vector<const DexFile*> dex_files;
    if (!boot_image_option.empty()) {
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      DexFile::OpenDexFiles(dex_filenames, dex_files, host_prefix);
      std::vector<const DexFile*> class_path_files(dex_files);
      OpenClassPathFiles(runtime_->GetClassPath(), class_path_files);
      for (size_t i = 0; i < class_path_files.size(); i++) {
        class_linker->RegisterDexFile(*class_path_files[i]);
      }
      class_loader.get()->reset(PathClassLoader::AllocCompileTime(class_path_files));
    } else {
      dex_files = runtime_->GetClassLinker()->GetBootClassPath();
    }

    Compiler compiler(instruction_set_, image, image_classes);
    compiler.CompileAll(class_loader->get(), dex_files);

    if (!OatWriter::Create(oat_file, class_loader->get(), compiler)) {
      LOG(ERROR) << "Failed to create oat file " << oat_file->name();
      return false;
    }
    return true;
  }

  bool CreateImageFile(const char* image_filename,
                       uintptr_t image_base,
                       const std::set<std::string>* image_classes,
                       const std::string& oat_filename,
                       const std::string& host_prefix) {
    // If we have an existing boot image, position new space after its oat file
    if (Heap::GetSpaces().size() > 1) {
      Space* last_image_space = Heap::GetSpaces()[Heap::GetSpaces().size()-2];
      CHECK(last_image_space != NULL);
      CHECK(last_image_space->IsImageSpace());
      CHECK(!Heap::GetSpaces()[Heap::GetSpaces().size()-1]->IsImageSpace());
      byte* oat_limit_addr = last_image_space->GetImageHeader().GetOatLimitAddr();
      image_base = RoundUp(reinterpret_cast<uintptr_t>(oat_limit_addr), kPageSize);
    }

    ImageWriter image_writer(image_classes);
    if (!image_writer.Write(image_filename, image_base, oat_filename, host_prefix)) {
      LOG(ERROR) << "Failed to create image file " << image_filename;
      return false;
    }
    return true;
  }

 private:

  Dex2Oat(Runtime* runtime) : runtime_(runtime) {}

  static Runtime* CreateRuntime(Runtime::Options& options) {
    Runtime* runtime = Runtime::Create(options, false);
    if (runtime == NULL) {
      LOG(ERROR) << "Failed to create runtime";
      return NULL;
    }

    // if we loaded an existing image, we will reuse values from the image roots.
    if (!runtime->HasJniDlsymLookupStub()) {
      runtime->SetJniDlsymLookupStub(Compiler::CreateJniDlysmLookupStub(instruction_set_));
    }
    if (!runtime->HasAbstractMethodErrorStubArray()) {
      runtime->SetAbstractMethodErrorStubArray(Compiler::CreateAbstractMethodErrorStub(instruction_set_));
    }
    for (int i = 0; i < Runtime::kLastTrampolineMethodType; i++) {
      Runtime::TrampolineType type = Runtime::TrampolineType(i);
      if (!runtime->HasResolutionStubArray(type)) {
        runtime->SetResolutionStubArray(Compiler::CreateResolutionStub(instruction_set_, type), type);
      }
    }
    for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
      Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
      if (!runtime->HasCalleeSaveMethod(type)) {
        runtime->SetCalleeSaveMethod(runtime->CreateCalleeSaveMethod(instruction_set_, type), type);
      }
    }
    return runtime;
  }

  static bool ClassVisitor(Class* klass, void* arg) {
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
      const DexFile* dex_file = DexFile::Open(parsed[i], Runtime::Current()->GetHostPrefix());
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

  Runtime* runtime_;
  static const InstructionSet instruction_set_ = kThumb2;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Dex2Oat);
};

int dex2oat(int argc, char** argv) {
  // Skip over argv[0].
  argv++;
  argc--;

  if (argc == 0) {
    fprintf(stderr, "no arguments specified\n");
    usage();
  }

  std::vector<const char*> dex_filenames;
  std::string oat_filename;
  const char* image_filename = NULL;
  const char* image_classes_filename = NULL;
  std::string boot_image_option;
  uintptr_t image_base = 0;
  std::string host_prefix;
  std::vector<const char*> runtime_args;

  for (int i = 0; i < argc; i++) {
    const StringPiece option(argv[i]);
    if (false) {
      LOG(INFO) << "dex2oat: option[" << i << "]=" << argv[i];
    }
    if (option.starts_with("--dex-file=")) {
      dex_filenames.push_back(option.substr(strlen("--dex-file=")).data());
    } else if (option.starts_with("--oat=")) {
      oat_filename = option.substr(strlen("--oat=")).data();
    } else if (option.starts_with("--image=")) {
      image_filename = option.substr(strlen("--image=")).data();
    } else if (option.starts_with("--image-classes=")) {
      image_classes_filename = option.substr(strlen("--image-classes=")).data();
    } else if (option.starts_with("--base=")) {
      const char* image_base_str = option.substr(strlen("--base=")).data();
      char* end;
      image_base = strtoul(image_base_str, &end, 16);
      if (end == image_base_str || *end != '\0') {
        fprintf(stderr, "Failed to parse hexadecimal value for option %s\n", option.data());
        usage();
      }
    } else if (option.starts_with("--boot-image=")) {
      const char* boot_image_filename = option.substr(strlen("--boot-image=")).data();
      boot_image_option.clear();
      boot_image_option += "-Ximage:";
      boot_image_option += boot_image_filename;
    } else if (option.starts_with("--host-prefix=")) {
      host_prefix = option.substr(strlen("--host-prefix=")).data();
    } else if (option == "--runtime-arg") {
      if (++i >= argc) {
        fprintf(stderr, "Missing required argument for --runtime-arg\n");
        usage();
      }
      runtime_args.push_back(argv[i]);
    } else {
      fprintf(stderr, "unknown argument %s\n", option.data());
      usage();
    }
  }

  if (oat_filename.empty()) {
    fprintf(stderr, "--oat file name not specified\n");
    return EXIT_FAILURE;
  }

  bool image = (image_filename != NULL);
  if (!image && boot_image_option.empty()) {
    fprintf(stderr, "Either --image or --boot-image must be specified\n");
    return EXIT_FAILURE;
  }

  if (image_classes_filename != NULL && !image) {
    fprintf(stderr, "--image-classes should only be used with --image\n");
    return EXIT_FAILURE;
  }

  if (image_classes_filename != NULL && !boot_image_option.empty()) {
    fprintf(stderr, "--image-classes should not be used with --boot-image\n");
    return EXIT_FAILURE;
  }

  if (boot_image_option.empty()) {
    if (image_base == 0) {
      fprintf(stderr, "non-zero --base not specified\n");
      return EXIT_FAILURE;
    }
  }

  // Create the output file if we can, or open it read-only if we weren't first.
  bool did_create = true;
  int fd = open(oat_filename.c_str(), O_EXCL | O_CREAT | O_TRUNC | O_RDWR, 0666);
  if (fd == -1) {
    if (errno != EEXIST) {
      PLOG(ERROR) << "Unable to create oat file " << oat_filename;
      return EXIT_FAILURE;
    }
    did_create = false;
    fd = open(oat_filename.c_str(), O_RDONLY);
    if (fd == -1) {
      PLOG(ERROR) << "Unable to open oat file for reading " << oat_filename;
      return EXIT_FAILURE;
    }
  }

  // Handles removing the file on failure and unlocking on both failure and success.
  FileJanitor oat_file_janitor(oat_filename, fd);

  // If we won the creation race, block trying to take the lock (since we're going to be doing
  // the work, we need the lock). If we lost the creation race, spin trying to take the lock
  // non-blocking until we fail -- at which point we know the other guy has the lock -- and then
  // block trying to take the now-taken lock.
  if (did_create) {
    LOG(INFO) << "This process created " << oat_filename;
    while (TEMP_FAILURE_RETRY(flock(fd, LOCK_EX)) != 0) {
      // Try again.
    }
    LOG(INFO) << "This process created and locked " << oat_filename;
  } else {
    LOG(INFO) << "Another process has already created " << oat_filename;
    while (TEMP_FAILURE_RETRY(flock(fd, LOCK_EX | LOCK_NB)) == 0) {
      // Give up the lock and hope the creator has taken the lock next time round.
      int rc = TEMP_FAILURE_RETRY(flock(fd, LOCK_UN));
      if (rc == -1) {
        PLOG(FATAL) << "Failed to unlock " << oat_filename;
      }
    }
    // Now a non-blocking attempt to take the lock has failed, we know the other guy has the
    // lock, so block waiting to take it.
    LOG(INFO) << "Another process is already working on " << oat_filename;
    if (TEMP_FAILURE_RETRY(flock(fd, LOCK_EX)) != 0) {
      PLOG(ERROR) << "Waiter unable to wait for creator to finish " << oat_filename;
      return EXIT_FAILURE;
    }
    // We have the lock and the creator has finished.
    // TODO: check the creator did a good job by checking the header.
    LOG(INFO) << "Another process finished working on " << oat_filename;
    // Job done.
    oat_file_janitor.KeepFile();
    return EXIT_SUCCESS;
  }

  // If we get this far, we won the creation race and have locked the file.
  UniquePtr<File> oat_file(OS::FileFromFd(oat_filename.c_str(), fd));

  LOG(INFO) << "dex2oat: " << oat_file->name();

  Runtime::Options options;
  options.push_back(std::make_pair("compiler", reinterpret_cast<void*>(NULL)));
  std::string boot_class_path_string;
  if (boot_image_option.empty()) {
    boot_class_path_string += "-Xbootclasspath:";
    for (size_t i = 0; i < dex_filenames.size()-1; i++) {
      boot_class_path_string += dex_filenames[i];
      boot_class_path_string += ":";
    }
    boot_class_path_string += dex_filenames[dex_filenames.size()-1];
    options.push_back(std::make_pair(boot_class_path_string.c_str(), reinterpret_cast<void*>(NULL)));
  } else {
    options.push_back(std::make_pair(boot_image_option.c_str(), reinterpret_cast<void*>(NULL)));
  }
  if (!host_prefix.empty()) {
    options.push_back(std::make_pair("host-prefix", host_prefix.c_str()));
  }
  for (size_t i = 0; i < runtime_args.size(); i++) {
    options.push_back(std::make_pair(runtime_args[i], reinterpret_cast<void*>(NULL)));
  }

  UniquePtr<Dex2Oat> dex2oat(Dex2Oat::Create(options));

  // If --image-classes was specified, calculate the full list classes to include in the image
  UniquePtr<const std::set<std::string> > image_classes(NULL);
  if (image_classes_filename != NULL) {
    image_classes.reset(dex2oat->GetImageClassDescriptors(image_classes_filename));
    if (image_classes.get() == NULL) {
      LOG(ERROR) << "Failed to create list of image classes from " << image_classes_filename;
      return EXIT_FAILURE;
    }
  }

  if (!dex2oat->CreateOatFile(boot_image_option,
                              dex_filenames,
                              host_prefix,
                              oat_file.get(),
                              image,
                              image_classes.get())) {
    LOG(ERROR) << "Failed to create oat file" << oat_filename;
    return EXIT_FAILURE;
  }

  if (!image) {
    oat_file_janitor.KeepFile();
    LOG(INFO) << "Oat file written successfully " << oat_filename;
    return EXIT_SUCCESS;
  }

  if (!dex2oat->CreateImageFile(image_filename,
                                image_base,
                                image_classes.get(),
                                oat_filename,
                                host_prefix)) {
    return EXIT_FAILURE;
  }

  // We wrote the oat file successfully, and want to keep it.
  oat_file_janitor.KeepFile();
  LOG(INFO) << "Oat file written successfully " << oat_filename;
  LOG(INFO) << "Image written successfully " << image_filename;
  return EXIT_SUCCESS;
}

} // namespace art

int main(int argc, char** argv) {
  return art::dex2oat(argc, argv);
}
