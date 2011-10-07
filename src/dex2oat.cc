// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "class_linker.h"
#include "class_loader.h"
#include "compiler.h"
#include "image_writer.h"
#include "oat_writer.h"
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
          "      Example: --image=/system/framework/boot.art\n"
          "\n");
  // TODO: remove this by inferring from --image
  fprintf(stderr,
          "  --oat=<file.oat>: specifies the required oat filename.\n"
          "      Example: --image=/system/framework/boot.oat\n"
          "\n");
  fprintf(stderr,
          "  --base=<hex-address>: specifies the base address when creating a boot image.\n"
          "      Example: --base=0x50000000\n"
          "\n");
  fprintf(stderr,
          "  --boot-image=<file.art>: provide the image file for the boot class path.\n"
          "      Example: --boot-image=/system/framework/boot.art\n"
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
          "  -Xms<n> may be used to specify an initial heap size for the runtime used to\n"
          "      run dex2oat\n"
          "      Example: -Xms256m\n"
          "\n");
  fprintf(stderr,
          "  -Xmx<n> may be used to specify a maximum heap size for the runtime used to\n"
          "      run dex2oat\n"
          "      Example: -Xmx256m\n"
          "\n");
  exit(EXIT_FAILURE);
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
  const char* Xms = NULL;
  const char* Xmx = NULL;

  for (int i = 0; i < argc; i++) {
    const StringPiece option(argv[i]);
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
    } else if (option.starts_with("-Xms")) {
      Xms = option.data();
    } else if (option.starts_with("-Xmx")) {
      Xmx = option.data();
    } else {
      fprintf(stderr, "unknown argument %s\n", option.data());
      usage();
    }
  }

  if (oat_filename == NULL) {
   fprintf(stderr, "--oat file name not specified\n");
   return EXIT_FAILURE;
  }

  if (image_filename == NULL) {
   fprintf(stderr, "--image file name not specified\n");
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

  Runtime::Options options;
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
  if (Xms != NULL) {
    options.push_back(std::make_pair(Xms, reinterpret_cast<void*>(NULL)));
  }
  if (Xmx != NULL) {
    options.push_back(std::make_pair(Xmx, reinterpret_cast<void*>(NULL)));
  }
  if (!host_prefix.empty()) {
    options.push_back(std::make_pair("host-prefix", host_prefix.c_str()));
  }
  UniquePtr<Runtime> runtime(Runtime::Create(options, false));
  if (runtime.get() == NULL) {
    fprintf(stderr, "could not create runtime\n");
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
  const ClassLoader* class_loader;
  if (boot_image_option.empty()) {
    class_loader = NULL;
  } else {
    std::vector<const DexFile*> dex_files;
    DexFile::OpenDexFiles(dex_filenames, dex_files, host_prefix);
    for (size_t i = 0; i < dex_files.size(); i++) {
      class_linker->RegisterDexFile(*dex_files[i]);
    }
    class_loader = PathClassLoader::Alloc(dex_files);
  }

  // if we loaded an existing image, we will reuse values from the image roots.
  if (!runtime->HasJniStubArray()) {
    runtime->SetJniStubArray(JniCompiler::CreateJniStub(kThumb2));
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

  if (!OatWriter::Create(oat_filename, class_loader)) {
    fprintf(stderr, "Failed to create oat file %s\n", oat_filename.c_str());
    return EXIT_FAILURE;
  }

  ImageWriter image_writer;
  if (!image_writer.Write(image_filename, image_base, oat_filename, host_prefix)) {
    fprintf(stderr, "Failed to create image file %s\n", image_filename);
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

} // namespace art

int main(int argc, char** argv) {
  return art::dex2oat(argc, argv);
}
