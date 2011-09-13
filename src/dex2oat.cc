// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "class_linker.h"
#include "class_loader.h"
#include "compiler.h"
#include "image_writer.h"
#include "runtime.h"
#include "stringpiece.h"

namespace art {

static void usage() {
  fprintf(stderr,
          "Usage: dex2oat [options]...\n"
          "\n");
  fprintf(stderr,
          "  --dex-file=<dex-file>: specifies a .dex files to compile. At least one .dex\n"
          "      but more than one may be included. \n"
          "      Example: --dex-file=/system/framework/core.jar\n"
          "\n");
  fprintf(stderr,
          "  --image=<file>: specifies the required output image filename.\n"
          "      Example: --image=/system/framework/boot.oat\n"
          "\n");
  fprintf(stderr,
          "  --base=<hex-address>: specifies the base address when creating a boot image.\n"
          "      Example: --base=0x50000000\n"
          "\n");
  fprintf(stderr,
          "  --boot=<oat-file>: provide the oat file for the boot class path.\n"
          "      Example: --boot=/system/framework/boot.oat\n"
          "\n");
  // TODO: remove this by making boot image contain boot DexFile information?
  fprintf(stderr,
          "  --boot-dex-file=<dex-file>: specifies a .dex file that is part of the boot\n"
          "       image specified with --boot. \n"
          "      Example: --boot-dex-file=/system/framework/core.jar\n"
          "\n");
  fprintf(stderr,
          "  --method may be used to limit compilation to a subset of methods.\n"
          "      Example: --method=Ljava/lang/Object;<init>()V\n"
          "\n");
  fprintf(stderr,
          "  --strip-prefix may be used to strip a path prefix from dex file names in the\n"
          "       the generated image to match the target file system layout.\n"
          "      Example: --strip-prefix=out/target/product/crespo\n"
          "\n");
  exit(EXIT_FAILURE);
}

static void OpenDexFiles(std::vector<const char*>& dex_filenames,
                         std::vector<const DexFile*>& dex_files,
                         const std::string& strip_location_prefix) {
  for (size_t i = 0; i < dex_filenames.size(); i++) {
    const char* dex_filename = dex_filenames[i];
    const DexFile* dex_file = DexFile::Open(dex_filename, strip_location_prefix);
    if (dex_file == NULL) {
      fprintf(stderr, "could not open .dex from file %s\n", dex_filename);
      exit(EXIT_FAILURE);
    }
    dex_files.push_back(dex_file);
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
  const char* image_filename = NULL;
  std::string boot_image_option;
  std::vector<const char*> boot_dex_filenames;
  uintptr_t image_base = 0;
  std::string strip_location_prefix;

  for (int i = 0; i < argc; i++) {
    const StringPiece option(argv[i]);
    if (option.starts_with("--dex-file=")) {
      dex_filenames.push_back(option.substr(strlen("--dex-file=")).data());
    } else if (option.starts_with("--method=")) {
      method_names.push_back(option.substr(strlen("--method=")).data());
    } else if (option.starts_with("--image=")) {
      image_filename = option.substr(strlen("--image=")).data();
    } else if (option.starts_with("--base=")) {
      const char* image_base_str = option.substr(strlen("--base=")).data();
      char* end;
      image_base = strtoul(image_base_str, &end, 16);
      if (end == image_base_str || *end != '\0') {
        fprintf(stderr, "could not parse hexadecimal value for option %s\n", option.data());
        usage();
      }
    } else if (option.starts_with("--boot=")) {
      const char* boot_image_filename = option.substr(strlen("--boot=")).data();
      boot_image_option.clear();
      boot_image_option += "-Xbootimage:";
      boot_image_option += boot_image_filename;
    } else if (option.starts_with("--boot-dex-file=")) {
      boot_dex_filenames.push_back(option.substr(strlen("--boot-dex-file=")).data());
    } else if (option.starts_with("--strip-prefix=")) {
      strip_location_prefix = option.substr(strlen("--strip-prefix=")).data();
    } else {
      fprintf(stderr, "unknown argument %s\n", option.data());
      usage();
    }
  }

  if (image_filename == NULL) {
   fprintf(stderr, "--image file name not specified\n");
   return EXIT_FAILURE;
  }

  if (boot_image_option.empty()) {
    if (image_base == 0) {
      fprintf(stderr, "non-zero --base not specified\n");
      return EXIT_FAILURE;
    }
  } else {
    if (boot_dex_filenames.empty()) {
      fprintf(stderr, "no --boot-dex-file specified with --boot\n");
      return EXIT_FAILURE;
    }
  }

  std::vector<const DexFile*> dex_files;
  OpenDexFiles(dex_filenames, dex_files, strip_location_prefix);

  std::vector<const DexFile*> boot_dex_files;
  OpenDexFiles(boot_dex_filenames, boot_dex_files, strip_location_prefix);

  Runtime::Options options;
  if (boot_image_option.empty()) {
    options.push_back(std::make_pair("bootclasspath", &dex_files));
  } else {
    options.push_back(std::make_pair("bootclasspath", &boot_dex_files));
    options.push_back(std::make_pair(boot_image_option.c_str(), reinterpret_cast<void*>(NULL)));
  }
  UniquePtr<Runtime> runtime(Runtime::Create(options, false));
  if (runtime.get() == NULL) {
    fprintf(stderr, "could not create runtime\n");
    return EXIT_FAILURE;
  }
  ClassLinker* class_linker = runtime->GetClassLinker();

  // If we have an existing boot image, position new space after it
  if (!boot_image_option.empty()) {
    Space* boot_space = Heap::GetBootSpace();
    CHECK(boot_space != NULL);
    image_base = RoundUp(reinterpret_cast<uintptr_t>(boot_space->GetLimit()), kPageSize);
  }

  // ClassLoader creation needs to come after Runtime::Create
  const ClassLoader* class_loader;
  if (boot_image_option.empty()) {
    class_loader = NULL;
  } else {
    for (size_t i = 0; i < dex_files.size(); i++) {
      class_linker->RegisterDexFile(*dex_files[i]);
    }
    class_loader = PathClassLoader::Alloc(dex_files);
  }

  // if we loaded an existing image, we will reuse its stub array.
  if (!runtime->HasJniStubArray()) {
    runtime->SetJniStubArray(JniCompiler::CreateJniStub(kThumb2));
  }

  Compiler compiler(kThumb2);
  if (method_names.empty()) {
    compiler.CompileAll(class_loader);
  } else {
    for (size_t i = 0; i < method_names.size(); i++) {
      // names are actually class_descriptor + name + signature.
      // example: Ljava/lang/Object;<init>()V
      StringPiece method_name = method_names[i];
      size_t end_of_class_descriptor = method_name.find(';');
      if (end_of_class_descriptor == method_name.npos) {
        fprintf(stderr, "could not find class descriptor in method %s\n", method_name.data());
        return EXIT_FAILURE;
      }
      end_of_class_descriptor++;  // want to include ;
      std::string class_descriptor = method_name.substr(0, end_of_class_descriptor).ToString();
      size_t end_of_name = method_name.find('(', end_of_class_descriptor);
      if (end_of_name == method_name.npos) {
        fprintf(stderr, "could not find start of method signature in method %s\n", method_name.data());
        return EXIT_FAILURE;
      }
      std::string name = method_name.substr(end_of_class_descriptor,
                                            end_of_name - end_of_class_descriptor).ToString();
      std::string signature = method_name.substr(end_of_name).ToString();

      Class* klass = class_linker->FindClass(class_descriptor, class_loader);
      if (klass == NULL) {
        fprintf(stderr, "could not find class for descriptor %s in method %s\n",
                class_descriptor.c_str(), method_name.data());
        return EXIT_FAILURE;
      }
      Method* method = klass->FindDirectMethod(name, signature);
      if (method == NULL) {
          method = klass->FindVirtualMethod(name, signature);
      }
      if (method == NULL) {
        fprintf(stderr, "could not find method %s with signature %s in class %s for method argument %s\n",
                name.c_str(),
                signature.c_str(),
                class_descriptor.c_str(),
                method_name.data());
        return EXIT_FAILURE;
      }
      compiler.CompileOne(method);
    }
  }

  ImageWriter writer;
  if (!writer.Write(image_filename, image_base)) {
    fprintf(stderr, "could not write image %s\n", image_filename);
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

} // namespace art

int main(int argc, char** argv) {
  return art::dex2oat(argc, argv);
}
