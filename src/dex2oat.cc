// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>

#include <string>
#include <vector>

#include "class_linker.h"
#include "class_loader.h"
#include "compiler.h"
#include "file.h"
#include "image_writer.h"
#include "oat_writer.h"
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
          "  --image=<file.art>: specifies the required output image filename.\n"
          "      Example: --image=/data/art-cache/boot.art\n"
          "\n");
  // TODO: remove this by inferring from --image
  fprintf(stderr,
          "  --oat=<file.oat>: specifies the required oat filename.\n"
          "      Example: --image=/data/art-cache/boot.oat\n"
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
          "  --method may be used to limit compilation to a subset of methods.\n"
          "      Example: --method=Ljava/lang/Object;<init>()V\n"
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

// Returns true if dex_files has a dex with the named location.
bool DexFilesContains(const std::vector<const DexFile*>& dex_files, const std::string& location) {
  for (size_t i = 0; i < dex_files.size(); ++i) {
    if (dex_files[i]->GetLocation() == location) {
      return true;
    }
  }
  return false;
}

// Appends to dex_files any elements of class_path that it doesn't already
// contain. This will open those dex files as necessary.
void OpenClassPathFiles(const std::string& class_path, std::vector<const DexFile*>& dex_files) {
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

int dex2oat(int argc, char** argv) {
  // Skip over argv[0].
  argv++;
  argc--;

  if (argc == 0) {
    fprintf(stderr, "no arguments specified\n");
    usage();
  }

  std::vector<const char*> dex_filenames;
  std::vector<const char*> method_names;
  std::string oat_filename;
  const char* image_filename = NULL;
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
    } else if (option.starts_with("--method=")) {
      method_names.push_back(option.substr(strlen("--method=")).data());
    } else if (option.starts_with("--oat=")) {
      oat_filename = option.substr(strlen("--oat=")).data();
    } else if (option.starts_with("--image=")) {
      image_filename = option.substr(strlen("--image=")).data();
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

  if (oat_filename == NULL) {
    fprintf(stderr, "--oat file name not specified\n");
    return EXIT_FAILURE;
  }

  if (image_filename == NULL && boot_image_option.empty()) {
    fprintf(stderr, "Either --image or --boot-image must be specified\n");
    return EXIT_FAILURE;
  }

  if (dex_filenames.empty()) {
    fprintf(stderr, "no --dex-file values specified\n");
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
  FileJanitor file_janitor(oat_filename, fd);

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
    file_janitor.KeepFile();
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
  UniquePtr<Runtime> runtime(Runtime::Create(options, false));
  if (runtime.get() == NULL) {
    LOG(ERROR) << "Could not create runtime";
    return EXIT_FAILURE;
  }
  ClassLinker* class_linker = runtime->GetClassLinker();

  // If we have an existing boot image, position new space after its oat file
  if (Heap::GetSpaces().size() > 1) {
    Space* last_image_space = Heap::GetSpaces()[Heap::GetSpaces().size()-2];
    CHECK(last_image_space != NULL);
    CHECK(last_image_space->IsImageSpace());
    CHECK(!Heap::GetSpaces()[Heap::GetSpaces().size()-1]->IsImageSpace());
    byte* oat_limit_addr = last_image_space->GetImageHeader().GetOatLimitAddr();
    image_base = RoundUp(reinterpret_cast<uintptr_t>(oat_limit_addr), kPageSize);
  }

  // ClassLoader creation needs to come after Runtime::Create
  SirtRef<ClassLoader> class_loader(NULL);
  std::vector<const DexFile*> dex_files;
  if (!boot_image_option.empty()) {
    DexFile::OpenDexFiles(dex_filenames, dex_files, host_prefix);
    std::vector<const DexFile*> class_path_files(dex_files);
    OpenClassPathFiles(runtime->GetClassPath(), class_path_files);
    for (size_t i = 0; i < class_path_files.size(); i++) {
      class_linker->RegisterDexFile(*class_path_files[i]);
    }
    class_loader.reset(PathClassLoader::AllocCompileTime(class_path_files));
  } else {
    dex_files = runtime->GetClassLinker()->GetBootClassPath();
  }

  // if we loaded an existing image, we will reuse values from the image roots.
  if (!runtime->HasJniDlsymLookupStub()) {
    runtime->SetJniDlsymLookupStub(Compiler::CreateJniDlysmLookupStub(kThumb2));
  }
  if (!runtime->HasAbstractMethodErrorStubArray()) {
    runtime->SetAbstractMethodErrorStubArray(Compiler::CreateAbstractMethodErrorStub(kThumb2));
  }
  for (int i = 0; i < Runtime::kLastTrampolineMethodType; i++) {
    Runtime::TrampolineType type = Runtime::TrampolineType(i);
    if (!runtime->HasResolutionStubArray(type)) {
      runtime->SetResolutionStubArray(Compiler::CreateResolutionStub(kThumb2, type), type);
    }
  }
  for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
    Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
    if (!runtime->HasCalleeSaveMethod(type)) {
      runtime->SetCalleeSaveMethod(runtime->CreateCalleeSaveMethod(kThumb2, type), type);
    }
  }
  Compiler compiler(kThumb2, image_filename != NULL);
  if (method_names.empty()) {
    compiler.CompileAll(class_loader.get(), dex_files);
  } else {
    for (size_t i = 0; i < method_names.size(); i++) {
      // names are actually class_descriptor + name + signature.
      // example: Ljava/lang/Object;<init>()V
      StringPiece method_name = method_names[i];
      size_t end_of_class_descriptor = method_name.find(';');
      if (end_of_class_descriptor == method_name.npos) {
        LOG(ERROR) << "Could not find class descriptor in method " << method_name << "'";
        return EXIT_FAILURE;
      }
      end_of_class_descriptor++;  // want to include ;
      std::string class_descriptor = method_name.substr(0, end_of_class_descriptor).ToString();
      size_t end_of_name = method_name.find('(', end_of_class_descriptor);
      if (end_of_name == method_name.npos) {
        LOG(ERROR) << "Could not find start of method signature in method '" << method_name << "'";
        return EXIT_FAILURE;
      }
      std::string name = method_name.substr(end_of_class_descriptor,
                                            end_of_name - end_of_class_descriptor).ToString();
      std::string signature = method_name.substr(end_of_name).ToString();

      Class* klass = class_linker->FindClass(class_descriptor, class_loader.get());
      if (klass == NULL) {
        LOG(ERROR) << "Could not find class for descriptor '" << class_descriptor
            << "' in method '" << method_name << "'";
        return EXIT_FAILURE;
      }
      Method* method = klass->FindDirectMethod(name, signature);
      if (method == NULL) {
          method = klass->FindVirtualMethod(name, signature);
      }
      if (method == NULL) {
        LOG(ERROR) << "Could not find method '" << method_name << "' with signature '"
            << signature << "' in class '" << class_descriptor << "' for method argument '"
            << method_name << "'";
        return EXIT_FAILURE;
      }
      compiler.CompileOne(method);
    }
  }

  if (!OatWriter::Create(oat_file.get(), class_loader.get(), compiler)) {
    LOG(ERROR) << "Failed to create oat file " << oat_file->name();
    return EXIT_FAILURE;
  }

  if (image_filename == NULL) {
    file_janitor.KeepFile();
    LOG(INFO) << "Oat file written successfully " << oat_file->name();
    return EXIT_SUCCESS;
  }
  CHECK(compiler.IsImage());

  ImageWriter image_writer;
  if (!image_writer.Write(image_filename, image_base, oat_file->name(), host_prefix)) {
    LOG(ERROR) << "Failed to create image file " << image_filename;
    return EXIT_FAILURE;
  }

  // We wrote the file successfully, and want to keep it.
  LOG(INFO) << "Image written successfully " << image_filename;
  file_janitor.KeepFile();
  return EXIT_SUCCESS;
}

} // namespace art

int main(int argc, char** argv) {
  return art::dex2oat(argc, argv);
}
