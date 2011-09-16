// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "class_linker.h"
#include "image.h"
#include "runtime.h"
#include "space.h"
#include "stringpiece.h"

namespace art {

static void usage() {
  fprintf(stderr,
          "Usage: oatdump [options] ...\n"
          "    Example: oatdump --dex-file=$ANDROID_PRODUCT_OUT/system/framework/core.jar --image=$ANDROID_PRODUCT_OUT/system/framework/boot.oat --strip-prefix=$ANDROID_PRODUCT_OUT\n"
          "    Example: adb shell oatdump --dex-file=/system/framework/core.jar --image=/system/framework/boot.oat\n"
          "\n");
  // TODO: remove this by making image contain boot DexFile information?
  fprintf(stderr,
          "  --dex-file=<dex-file>: specifies a .dex file location. At least one .dex\n"
          "      file must be specified. \n"
          "      Example: --dex-file=/system/framework/core.jar\n"
          "\n");
  fprintf(stderr,
          "  --image=<file>: specifies the required input image filename.\n"
          "      Example: --image=/system/framework/boot.oat\n"
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
          "  --strip-prefix may be used to strip a path prefix from dex file names in the\n"
          "       the generated image to match the target file system layout.\n"
          "      Example: --strip-prefix=out/target/product/crespo\n"
          "\n");
  exit(EXIT_FAILURE);
}

const char* image_roots_descriptions_[ImageHeader::kImageRootsMax] = {
  "kJniStubArray",
};

struct OatDump {
  const Space* dump_space;

  bool InDumpSpace(const Object* object) {
    DCHECK(dump_space != NULL);
    const byte* o = reinterpret_cast<const byte*>(object);
    return (o >= dump_space->GetBase() && o < dump_space->GetLimit());
  }

  static void Callback(Object* obj, void* arg) {
    DCHECK(obj != NULL);
    DCHECK(arg != NULL);
    OatDump* state = reinterpret_cast<OatDump*>(arg);
    if (!state->InDumpSpace(obj)) {
      return;
    }
    std::string summary;
    StringAppendF(&summary, "%p: ", obj);
    if (obj->IsClass()) {
      Class* klass = obj->AsClass();
      StringAppendF(&summary, "CLASS %s", klass->GetDescriptor()->ToModifiedUtf8().c_str());
    } else if (obj->IsMethod()) {
      Method* method = obj->AsMethod();
      StringAppendF(&summary, "METHOD %s", PrettyMethod(method).c_str());
    } else if (obj->IsField()) {
      Field* field = obj->AsField();
      Class* type = field->GetType();
      std::string type_string;
      type_string += (type == NULL) ? "<UNKNOWN>" : type->GetDescriptor()->ToModifiedUtf8();
      StringAppendF(&summary, "FIELD %s", PrettyField(field).c_str());
    } else if (obj->IsArrayInstance()) {
      StringAppendF(&summary, "ARRAY %d", obj->AsArray()->GetLength());
    } else if (obj->IsString()) {
      StringAppendF(&summary, "STRING %s", obj->AsString()->ToModifiedUtf8().c_str());
    } else {
      StringAppendF(&summary, "OBJECT");
    }
    StringAppendF(&summary, "\n");
    StringAppendF(&summary, "\tclass %p: %s\n",
                  obj->GetClass(), obj->GetClass()->GetDescriptor()->ToModifiedUtf8().c_str());
    if (obj->IsMethod()) {
      Method* method = obj->AsMethod();
      const ByteArray* code = method->GetCodeArray();
      StringAppendF(&summary, "\tCODE     %p-%p\n", code->GetData(), code->GetData() + code->GetLength());
      const ByteArray* invoke = method->GetInvokeStubArray();
      StringAppendF(&summary, "\tJNI STUB %p-%p\n", invoke->GetData(), invoke->GetData() + invoke->GetLength());
      if (method->IsNative()) {
        if (method->IsRegistered()) {
         StringAppendF(&summary, "\tNATIVE REGISTERED %p\n", method->GetNativeMethod());
        } else {
          StringAppendF(&summary, "\tNATIVE UNREGISTERED\n");
        }
      }
    }
    std::cout << summary;
  }
};

int oatdump(int argc, char** argv) {
  // Skip over argv[0].
  argv++;
  argc--;

  if (argc == 0) {
    fprintf(stderr, "no arguments specified\n");
    usage();
  }

  std::vector<const char*> dex_filenames;
  const char* image_filename = NULL;
  const char* boot_image_filename = NULL;
  std::vector<const char*> boot_dex_filenames;
  std::string strip_location_prefix;

  for (int i = 0; i < argc; i++) {
    const StringPiece option(argv[i]);
    if (option.starts_with("--dex-file=")) {
      dex_filenames.push_back(option.substr(strlen("--dex-file=")).data());
    } else if (option.starts_with("--image=")) {
      image_filename = option.substr(strlen("--image=")).data();
    } else if (option.starts_with("--boot=")) {
      boot_image_filename = option.substr(strlen("--boot=")).data();
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

  if (dex_filenames.empty()) {
   fprintf(stderr, "no --dex-file values specified\n");
   return EXIT_FAILURE;
  }

  if (boot_image_filename != NULL && boot_dex_filenames.empty()) {
    fprintf(stderr, "no --boot-dex-file values specified with --boot\n");
    return EXIT_FAILURE;
  }

  std::vector<const DexFile*> dex_files;
  DexFile::OpenDexFiles(dex_filenames, dex_files, strip_location_prefix);

  std::vector<const DexFile*> boot_dex_files;
  DexFile::OpenDexFiles(boot_dex_filenames, boot_dex_files, strip_location_prefix);

  Runtime::Options options;
  std::string image_option;
  std::string boot_image_option;
  if (boot_image_filename == NULL) {
    // if we don't have multiple images, pass the main one as the boot to match dex2oat
    boot_image_filename = image_filename;
    boot_dex_files = dex_files;
    image_filename = NULL;
    dex_files.clear();
  } else {
    image_option += "-Ximage:";
    image_option += image_filename;
    options.push_back(std::make_pair("classpath", &dex_files));
    options.push_back(std::make_pair(image_option.c_str(), reinterpret_cast<void*>(NULL)));
  }
  boot_image_option += "-Xbootimage:";
  boot_image_option += boot_image_filename;
  options.push_back(std::make_pair("bootclasspath", &boot_dex_files));
  options.push_back(std::make_pair(boot_image_option.c_str(), reinterpret_cast<void*>(NULL)));

  UniquePtr<Runtime> runtime(Runtime::Create(options, false));
  if (runtime.get() == NULL) {
    fprintf(stderr, "could not create runtime\n");
    return EXIT_FAILURE;
  }
  ClassLinker* class_linker = runtime->GetClassLinker();
  for (size_t i = 0; i < dex_files.size(); i++) {
    class_linker->RegisterDexFile(*dex_files[i]);
  }

  Space* image_space = Heap::GetSpaces()[Heap::GetSpaces().size()-2];
  CHECK(image_space != NULL);
  const ImageHeader& image_header = image_space->GetImageHeader();
  if (!image_header.IsValid()) {
    fprintf(stderr, "invalid image header %s\n", image_filename);
    return EXIT_FAILURE;
  }

  printf("MAGIC:\n");
  printf("%s\n\n", image_header.GetMagic());

  printf("ROOTS:\n");
  for (int i = 0; i < ImageHeader::kImageRootsMax; i++) {
    ImageHeader::ImageRoot image_root = static_cast<ImageHeader::ImageRoot>(i);
    printf("%s: %p\n", image_roots_descriptions_[i], image_header.GetImageRoot(image_root));
  }
  printf("\n");

  printf("OBJECTS:\n");
  OatDump state;
  state.dump_space = image_space;
  HeapBitmap* heap_bitmap = Heap::GetLiveBits();
  DCHECK(heap_bitmap != NULL);
  heap_bitmap->Walk(OatDump::Callback, &state);

  return EXIT_SUCCESS;
}

} // namespace art

int main(int argc, char** argv) {
  return art::oatdump(argc, argv);
}
