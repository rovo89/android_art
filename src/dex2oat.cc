// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdio.h>
#include <stdlib.h>

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
#include "stringpiece.h"
#include "timing_logger.h"
#include "zip_archive.h"

namespace art {

static void usage() {
  fprintf(stderr,
          "Usage: dex2oat [options]...\n"
          "\n");
  fprintf(stderr,
          "  --dex-file=<dex-file>: specifies a .dex file to compile.\n"
          "      Example: --dex-file=/system/framework/core.jar\n"
          "\n");
  fprintf(stderr,
          "  --zip-fd=<file-descriptor>: specifies a file descriptor of a zip file\n"
          "      containing a classes.dex file to compile.\n"
          "      Example: --zip-fd=5\n"
          "\n");
  fprintf(stderr,
          "  --zip-name=<zip-name>: specifies a symbolic name for the file corresponding\n"
          "      to the file descriptor specified by --zip-fd.\n"
          "      Example: --zip-name=/system/app/Calculator.apk\n"
          "\n");
  fprintf(stderr,
          "  --oat-file=<file.oat>: specifies the required oat filename.\n"
          "      Example: --oat-file=/system/framework/boot.oat\n"
          "\n");
  fprintf(stderr,
          "  --oat-name=<oat-name>: specifies a symbolic name for the file corresponding\n"
          "      to the file descriptor specified by --oat-fd.\n"
          "      Example: --oat-name=/data/art-cache/system@app@Calculator.apk.oat\n"
          "\n");
  fprintf(stderr,
          "  --image=<file.art>: specifies the output image filename.\n"
          "      Example: --image=/system/framework/boot.art\n"
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
          "      Example: --boot-image=/system/framework/boot.art\n"
          "      Default: <host-prefix>/system/framework/boot.art\n"
          "\n");
  fprintf(stderr,
          "  --host-prefix may be used to translate host paths to target paths during\n"
          "      cross compilation.\n"
          "      Example: --host-prefix=out/target/product/crespo\n"
          "      Default: $ANDROID_PRODUCT_OUT\n"
          "\n");
  fprintf(stderr,
          "  --runtime-arg <argument>: used to specify various arguments for the runtime,\n"
          "      such as initial heap size, maximum heap size, and verbose output.\n"
          "      Use a separate --runtime-arg switch for each argument.\n"
          "      Example: --runtime-arg -Xms256m\n"
          "\n");
  exit(EXIT_FAILURE);
}

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
    LOG(INFO) << "dex2oat took " << NsToMs(NanoTime() - start_ns_) << "ms";
  }

  // Make a list of descriptors for classes to include in the image
  const std::set<std::string>* GetImageClassDescriptors(const char* image_classes_filename) {
    UniquePtr<std::ifstream> image_classes_file(new std::ifstream(image_classes_filename, std::ifstream::in));
    if (image_classes_file.get() == NULL) {
      LOG(ERROR) << "Failed to open image classes file " << image_classes_filename;
      return NULL;
    }

    // Load all the classes specified in the file
    ClassLinker* class_linker = runtime_->GetClassLinker();
    while (image_classes_file->good()) {
      std::string dot;
      std::getline(*image_classes_file.get(), dot);
      if (StringPiece(dot).starts_with("#") || dot.empty()) {
        continue;
      }
      std::string descriptor(DotToDescriptor(dot.c_str()));
      SirtRef<Class> klass(class_linker->FindSystemClass(descriptor.c_str()));
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
        SirtRef<Class> klass(class_linker->ResolveType(*dex_file, exception_type_idx, dex_cache,
                                                       class_loader));
        if (klass.get() == NULL) {
          const DexFile::TypeId& type_id = dex_file->GetTypeId(exception_type_idx);
          const char* descriptor = dex_file->GetTypeDescriptor(type_id);
          LOG(FATAL) << "Failed to resolve class " << descriptor;
        }
        DCHECK(klass->IsThrowableClass());
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

  bool CreateOatFile(const std::string& boot_image_option,
                     const std::vector<const DexFile*>& dex_files,
                     File* oat_file,
                     bool image,
                     const std::set<std::string>* image_classes) {
    // SirtRef and ClassLoader creation needs to come after Runtime::Create
    UniquePtr<SirtRef<ClassLoader> > class_loader(new SirtRef<ClassLoader>(NULL));
    if (class_loader.get() == NULL) {
      LOG(ERROR) << "Failed to create SirtRef for class loader";
      return false;
    }

    if (!boot_image_option.empty()) {
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      std::vector<const DexFile*> class_path_files(dex_files);
      OpenClassPathFiles(runtime_->GetClassPath(), class_path_files);
      for (size_t i = 0; i < class_path_files.size(); i++) {
        class_linker->RegisterDexFile(*class_path_files[i]);
      }
      class_loader.get()->reset(PathClassLoader::AllocCompileTime(class_path_files));
    }

    Compiler compiler(instruction_set_, image, image_classes);
    compiler.CompileAll(class_loader->get(), dex_files);

    if (!OatWriter::Create(oat_file, class_loader->get(), dex_files, compiler)) {
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

  explicit Dex2Oat(Runtime* runtime) : runtime_(runtime), start_ns_(NanoTime()) {
  }

  static Runtime* CreateRuntime(Runtime::Options& options) {
    Runtime* runtime = Runtime::Create(options, false);
    if (runtime == NULL) {
      LOG(ERROR) << "Failed to create runtime";
      return NULL;
    }

    // if we loaded an existing image, we will reuse values from the image roots.
    if (!runtime->HasJniDlsymLookupStub()) {
      runtime->SetJniDlsymLookupStub(Compiler::CreateJniDlsymLookupStub(instruction_set_));
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

  static void ResolveExceptionsForMethod(MethodHelper* mh,
                           std::set<std::pair<uint16_t, const DexFile*> >& exceptions_to_resolve) {
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
  static bool ResolveCatchBlockExceptionsClassVisitor(Class* c, void* arg) {
    std::set<std::pair<uint16_t, const DexFile*> >* exceptions_to_resolve =
        reinterpret_cast<std::set<std::pair<uint16_t, const DexFile*> >*>(arg);
    MethodHelper mh;
    for (size_t i = 0; i < c->NumVirtualMethods(); ++i) {
      Method* m = c->GetVirtualMethod(i);
      mh.ChangeMethod(m);
      ResolveExceptionsForMethod(&mh, *exceptions_to_resolve);
    }
    for (size_t i = 0; i < c->NumDirectMethods(); ++i) {
      Method* m = c->GetDirectMethod(i);
      mh.ChangeMethod(m);
      ResolveExceptionsForMethod(&mh, *exceptions_to_resolve);
    }
    return true;
  }
  static bool RecordImageClassesVisitor(Class* klass, void* arg) {
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

  static const InstructionSet instruction_set_ = kThumb2;

  Runtime* runtime_;
  uint64_t start_ns_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Dex2Oat);
};

bool ParseInt(const char* in, int* out) {
  char* end;
  int result = strtol(in, &end, 10);
  if (in == end || *end != '\0') {
    return false;
  }
  *out = result;
  return true;
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
  int zip_fd = -1;
  std::string zip_name;
  std::string oat_filename;
  int oat_fd = -1;
  std::string oat_name;
  const char* image_filename = NULL;
  const char* image_classes_filename = NULL;
  std::string boot_image_filename;
  uintptr_t image_base = 0;
  std::string host_prefix;
  std::vector<const char*> runtime_args;

  for (int i = 0; i < argc; i++) {
    const StringPiece option(argv[i]);
    bool log_options = false;
    if (log_options) {
      LOG(INFO) << "dex2oat: option[" << i << "]=" << argv[i];
    }
    if (option.starts_with("--dex-file=")) {
      dex_filenames.push_back(option.substr(strlen("--dex-file=")).data());
    } else if (option.starts_with("--zip-fd=")) {
      const char* zip_fd_str = option.substr(strlen("--zip-fd=")).data();
      if (!ParseInt(zip_fd_str, &zip_fd)) {
        fprintf(stderr, "could not parse --zip-fd argument '%s' as an integer\n", zip_fd_str);
        usage();
      }
    } else if (option.starts_with("--zip-name=")) {
      zip_name = option.substr(strlen("--zip-name=")).data();
    } else if (option.starts_with("--oat-file=")) {
      oat_filename = option.substr(strlen("--oat-file=")).data();
    } else if (option.starts_with("--oat-fd=")) {
      const char* oat_fd_str = option.substr(strlen("--oat-fd=")).data();
      if (!ParseInt(oat_fd_str, &oat_fd)) {
        fprintf(stderr, "could not parse --oat-fd argument '%s' as an integer\n", oat_fd_str);
        usage();
      }
    } else if (option.starts_with("--oat-name=")) {
      oat_name = option.substr(strlen("--oat-name=")).data();
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
      boot_image_filename = option.substr(strlen("--boot-image=")).data();
    } else if (option.starts_with("--host-prefix=")) {
      host_prefix = option.substr(strlen("--host-prefix=")).data();
    } else if (option == "--runtime-arg") {
      if (++i >= argc) {
        fprintf(stderr, "Missing required argument for --runtime-arg\n");
        usage();
      }
      if (log_options) {
        LOG(INFO) << "dex2oat: option[" << i << "]=" << argv[i];
      }
      runtime_args.push_back(argv[i]);
    } else {
      fprintf(stderr, "unknown argument %s\n", option.data());
      usage();
    }
  }

  if (oat_filename.empty() && oat_fd == -1) {
    fprintf(stderr, "Output must be supplied with either --oat-file or --oat-fd\n");
    return EXIT_FAILURE;
  }

  if (!oat_filename.empty() && oat_fd != -1) {
    fprintf(stderr, "--oat-file should not be used with --oat-fd\n");
    return EXIT_FAILURE;
  }

  if (!oat_filename.empty() && oat_fd != -1) {
    fprintf(stderr, "--oat-file should not be used with --oat-fd\n");
    return EXIT_FAILURE;
  }

  if (!oat_filename.empty() && !oat_name.empty()) {
    fprintf(stderr, "--oat-file should not be used with --oat-name\n");
    return EXIT_FAILURE;
  }

  if (oat_fd != -1 && oat_name.empty()) {
    fprintf(stderr, "--oat-name should be supplied with --oat-fd\n");
    return EXIT_FAILURE;
  }

  if (oat_fd != -1 && image_filename != NULL) {
    fprintf(stderr, "--oat-fd should not be used with --image\n");
    return EXIT_FAILURE;
  }

  if (host_prefix.empty()) {
    const char* android_product_out = getenv("ANDROID_PRODUCT_OUT");
    if (android_product_out != NULL) {
        host_prefix = android_product_out;
    }
  }

  bool image = (image_filename != NULL);
  if (!image && boot_image_filename.empty()) {
    boot_image_filename += host_prefix;
    boot_image_filename += "/system/framework/boot.art";
  }
  std::string boot_image_option;
  if (boot_image_filename != NULL) {
    boot_image_option += "-Ximage:";
    boot_image_option += boot_image_filename;
  }

  if (image_classes_filename != NULL && !image) {
    fprintf(stderr, "--image-classes should only be used with --image\n");
    return EXIT_FAILURE;
  }

  if (image_classes_filename != NULL && !boot_image_option.empty()) {
    fprintf(stderr, "--image-classes should not be used with --boot-image\n");
    return EXIT_FAILURE;
  }

  if (dex_filenames.empty() && zip_fd == -1) {
    fprintf(stderr, "Input must be supplied with either --dex-file or --zip-fd\n");
    return EXIT_FAILURE;
  }

  if (!dex_filenames.empty() && zip_fd != -1) {
    fprintf(stderr, "--dex-file should not be used with --zip-fd\n");
    return EXIT_FAILURE;
  }

  if (!dex_filenames.empty() && !zip_name.empty()) {
    fprintf(stderr, "--dex-file should not be used with --zip-name\n");
    return EXIT_FAILURE;
  }

  if (zip_fd != -1 && zip_name.empty()) {
    fprintf(stderr, "--zip-name should be supplied with --zip-fd\n");
    return EXIT_FAILURE;
  }

  if (boot_image_option.empty()) {
    if (image_base == 0) {
      fprintf(stderr, "non-zero --base not specified\n");
      return EXIT_FAILURE;
    }
  }

  // Check early that the result of compilation can be written
  UniquePtr<File> oat_file;
  if (!oat_filename.empty()) {
    oat_file.reset(OS::OpenFile(oat_filename.c_str(), true));
    oat_name = oat_filename;
  } else {
    oat_file.reset(OS::FileFromFd(oat_name.c_str(), oat_fd));
  }
  if (oat_file.get() == NULL) {
    PLOG(ERROR) << "Unable to create oat file: " << oat_name;
    return EXIT_FAILURE;
  }

  LOG(INFO) << "dex2oat: " << oat_name;

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
        LOG(ERROR) << "Failed to zip from file descriptor for " << zip_name;
        return EXIT_FAILURE;
      }
      const DexFile* dex_file = DexFile::Open(*zip_archive.get(), zip_name);
      if (dex_file == NULL) {
        LOG(ERROR) << "Failed to open dex from file descriptor for zip file: " << zip_name;
        return EXIT_FAILURE;
      }
      dex_files.push_back(dex_file);
    } else {
      DexFile::OpenDexFiles(dex_filenames, dex_files, host_prefix);
    }
  }

  if (!dex2oat->CreateOatFile(boot_image_option,
                              dex_files,
                              oat_file.get(),
                              image,
                              image_classes.get())) {
    LOG(ERROR) << "Failed to create oat file: " << oat_name;
    return EXIT_FAILURE;
  }

  if (!image) {
    LOG(INFO) << "Oat file written successfully: " << oat_name;
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
  LOG(INFO) << "Oat file written successfully: " << oat_filename;
  LOG(INFO) << "Image written successfully: " << image_filename;
  return EXIT_SUCCESS;
}

} // namespace art

int main(int argc, char** argv) {
  return art::dex2oat(argc, argv);
}
