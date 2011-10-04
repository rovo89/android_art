// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "class_linker.h"
#include "file.h"
#include "image.h"
#include "os.h"
#include "runtime.h"
#include "space.h"
#include "stringpiece.h"
#include "unordered_map.h"

namespace art {

static void usage() {
  fprintf(stderr,
          "Usage: oatdump [options] ...\n"
          "    Example: oatdump --image=$ANDROID_PRODUCT_OUT/system/framework/boot.art --host-prefix=$ANDROID_PRODUCT_OUT\n"
          "    Example: adb shell oatdump --image=/system/framework/boot.art\n"
          "\n");
  fprintf(stderr,
          "  --image=<file.art>: specifies the required input image filename.\n"
          "      Example: --image=/system/framework/boot.art\n"
          "\n");
  fprintf(stderr,
          "  --boot-image=<file.art>: provide the image file for the boot class path.\n"
          "      Example: --boot-image=/system/framework/boot.art\n"
          "\n");
  fprintf(stderr,
          "  --host-prefix may be used to translate host paths to target paths during\n"
          "      cross compilation.\n"
          "      Example: --host-prefix=out/target/product/crespo\n"
          "\n");
  fprintf(stderr,
          "  --output=<file> may be used to send the output to a file.\n"
          "      Example: --output=/tmp/oatdump.txt\n"
          "\n");
  exit(EXIT_FAILURE);
}

const char* image_roots_descriptions_[] = {
  "kJniStubArray",
  "kAbstractMethodErrorStubArray",
  "kCalleeSaveMethod",
  "kOatLocation",
  "kDexCaches",
};

class OatDump {

 public:
  static void Dump(const std::string& image_filename,
                   std::ostream& os,
                   Space& image_space,
                   const ImageHeader& image_header) {
    os << "MAGIC:\n";
    os << image_header.GetMagic() << "\n\n";

    os << "IMAGE BASE:\n";
    os << reinterpret_cast<void*>(image_header.GetImageBaseAddr()) << "\n\n";

    os << "OAT BASE:\n";
    os << reinterpret_cast<void*>(image_header.GetOatBaseAddr()) << "\n\n";

    os << "ROOTS:\n";
    CHECK_EQ(arraysize(image_roots_descriptions_), size_t(ImageHeader::kImageRootsMax));
    for (int i = 0; i < ImageHeader::kImageRootsMax; i++) {
      ImageHeader::ImageRoot image_root = static_cast<ImageHeader::ImageRoot>(i);
      os << StringPrintf("%s: %p\n",
                         image_roots_descriptions_[i], image_header.GetImageRoot(image_root));
    }
    os << "\n";

    os << "OBJECTS:\n" << std::flush;
    OatDump state(image_space, os);
    HeapBitmap* heap_bitmap = Heap::GetLiveBits();
    DCHECK(heap_bitmap != NULL);
    heap_bitmap->Walk(OatDump::Callback, &state);
    os << "\n";

    os << "STATS:\n" << std::flush;
    UniquePtr<File> file(OS::OpenFile(image_filename.c_str(), false));
    state.stats_.file_bytes = file->Length();
    size_t header_bytes = sizeof(ImageHeader);
    state.stats_.header_bytes = header_bytes;
    size_t alignment_bytes = RoundUp(header_bytes, kObjectAlignment) - header_bytes;
    state.stats_.alignment_bytes += alignment_bytes;
    state.stats_.Dump(os);

    os << std::flush;
  }

 private:

  OatDump(const Space& dump_space, std::ostream& os) : dump_space_(dump_space), os_(os) {
  }

  ~OatDump() {
  }

  static void Callback(Object* obj, void* arg) {
    DCHECK(obj != NULL);
    DCHECK(arg != NULL);
    OatDump* state = reinterpret_cast<OatDump*>(arg);
    if (!state->InDumpSpace(obj)) {
      return;
    }

    size_t object_bytes = obj->SizeOf();
    size_t alignment_bytes = RoundUp(object_bytes, kObjectAlignment) - object_bytes;
    state->stats_.object_bytes += object_bytes;
    state->stats_.alignment_bytes += alignment_bytes;

    std::string summary;
    StringAppendF(&summary, "%p: ", obj);
    if (obj->IsClass()) {
      Class* klass = obj->AsClass();
      StringAppendF(&summary, "CLASS %s", klass->GetDescriptor()->ToModifiedUtf8().c_str());
      std::stringstream ss;
      ss << " (" << klass->GetStatus() << ")";
      summary += ss.str();
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
    std::string descriptor = obj->GetClass()->GetDescriptor()->ToModifiedUtf8();
    StringAppendF(&summary, "\tclass %p: %s\n", obj->GetClass(), descriptor.c_str());
    state->stats_.descriptor_to_bytes[descriptor] += object_bytes;
    state->stats_.descriptor_to_count[descriptor] += 1;
    // StringAppendF(&summary, "\tsize %d (alignment padding %d)\n",
    //               object_bytes, RoundUp(object_bytes, kObjectAlignment) - object_bytes);
    if (obj->IsMethod()) {
      Method* method = obj->AsMethod();
      if (!method->IsPhony()) {
        const ByteArray* code = method->GetCodeArray();
        const int8_t* code_base = NULL;
        const int8_t* code_limit = NULL;
        if (code != NULL) {
          size_t code_bytes = code->GetLength();
          code_base = code->GetData();
          code_limit = code_base + code_bytes;
          if (method->IsNative()) {
            state->stats_.managed_to_native_code_bytes += code_bytes;
          } else {
            state->stats_.managed_code_bytes += code_bytes;
          }
        } else {
          code_base = reinterpret_cast<const int8_t*>(method->GetCode());
        }
        StringAppendF(&summary, "\tCODE     %p-%p\n", code_base, code_limit);

        const ByteArray* invoke = method->GetInvokeStubArray();
        const int8_t* invoke_base = NULL;
        const int8_t* invoke_limit = NULL;
        if (invoke != NULL) {
          size_t native_to_managed_code_bytes = invoke->GetLength();
          invoke_base = invoke->GetData();
          invoke_limit = invoke_base + native_to_managed_code_bytes;
          state->stats_.native_to_managed_code_bytes += native_to_managed_code_bytes;
          StringAppendF(&summary, "\tJNI STUB %p-%p\n", invoke_base, invoke_limit);
        }
      }
      if (method->IsNative()) {
        if (method->IsRegistered()) {
          StringAppendF(&summary, "\tNATIVE REGISTERED %p\n", method->GetNativeMethod());
        } else {
          StringAppendF(&summary, "\tNATIVE UNREGISTERED\n");
        }
        DCHECK(method->GetRegisterMapHeader() == NULL);
        DCHECK(method->GetRegisterMapData() == NULL);
        DCHECK(method->GetMappingTable() == NULL);
      } else if (method->IsAbstract()) {
        StringAppendF(&summary, "\tABSTRACT\n");
        DCHECK(method->GetRegisterMapHeader() == NULL);
        DCHECK(method->GetRegisterMapData() == NULL);
        DCHECK(method->GetMappingTable() == NULL);
      } else if (method->IsPhony()) {
        StringAppendF(&summary, "\tPHONY\n");
        DCHECK(method->GetRegisterMapHeader() == NULL);
        DCHECK(method->GetRegisterMapData() == NULL);
        DCHECK(method->GetMappingTable() == NULL);
      } else {
        size_t register_map_bytes = (method->GetRegisterMapHeader()->GetLength() +
                                     method->GetRegisterMapData()->GetLength());
        state->stats_.register_map_bytes += register_map_bytes;

        if (method->GetMappingTable() != NULL) {
          size_t pc_mapping_table_bytes = method->GetMappingTable()->GetLength();
          state->stats_.pc_mapping_table_bytes += pc_mapping_table_bytes;
        }

        ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
        class DexCache* dex_cache = method->GetDeclaringClass()->GetDexCache();
        const DexFile& dex_file = class_linker->FindDexFile(dex_cache);
        const DexFile::CodeItem* code_item = dex_file.GetCodeItem(method->GetCodeItemOffset());
        size_t dex_instruction_bytes = code_item->insns_size_ * 2;
        state->stats_.dex_instruction_bytes += dex_instruction_bytes;
      }
    }
    state->os_ << summary << std::flush;
  }

  bool InDumpSpace(const Object* object) {
    const byte* o = reinterpret_cast<const byte*>(object);
    return (o >= dump_space_.GetBase() && o < dump_space_.GetLimit());
  }

 public:
  struct Stats {
    size_t file_bytes;

    size_t header_bytes;
    size_t object_bytes;
    size_t alignment_bytes;

    size_t managed_code_bytes;
    size_t managed_to_native_code_bytes;
    size_t native_to_managed_code_bytes;

    size_t register_map_bytes;
    size_t pc_mapping_table_bytes;

    size_t dex_instruction_bytes;

    Stats()
        : file_bytes(0),
          header_bytes(0),
          object_bytes(0),
          alignment_bytes(0),
          managed_code_bytes(0),
          managed_to_native_code_bytes(0),
          native_to_managed_code_bytes(0),
          register_map_bytes(0),
          pc_mapping_table_bytes(0),
          dex_instruction_bytes(0) {}

    typedef std::tr1::unordered_map<std::string,size_t> TableBytes;
    TableBytes descriptor_to_bytes;

    typedef std::tr1::unordered_map<std::string,size_t> TableCount;
    TableCount descriptor_to_count;

    double PercentOfFileBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(file_bytes)) * 100;
    }

    double PercentOfObjectBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(object_bytes)) * 100;
    }

    void Dump(std::ostream& os) {
      os << StringPrintf("\tfile_bytes = %d\n", file_bytes);
      os << "\n";

      os << "\tfile_bytes = header_bytes + object_bytes + alignment_bytes\n";
      os << StringPrintf("\theader_bytes    = %10d (%2.0f%% of file_bytes)\n",
                         header_bytes, PercentOfFileBytes(header_bytes));
      os << StringPrintf("\tobject_bytes    = %10d (%2.0f%% of file_bytes)\n",
                         object_bytes, PercentOfFileBytes(object_bytes));
      os << StringPrintf("\talignment_bytes = %10d (%2.0f%% of file_bytes)\n",
                         alignment_bytes, PercentOfFileBytes(alignment_bytes));
      os << "\n";
      os << std::flush;
      CHECK_EQ(file_bytes, header_bytes + object_bytes + alignment_bytes);

      os << "\tobject_bytes = sum of descriptor_to_bytes values below:\n";
      size_t object_bytes_total = 0;
      typedef TableBytes::const_iterator It;  // TODO: C++0x auto
      for (It it = descriptor_to_bytes.begin(), end = descriptor_to_bytes.end(); it != end; ++it) {
        const std::string& descriptor = it->first;
        size_t bytes = it->second;
        size_t count = descriptor_to_count[descriptor];
        double average = static_cast<double>(bytes) / static_cast<double>(count);
        double percent = PercentOfObjectBytes(bytes);
        os << StringPrintf("\t%32s %8d bytes %6d instances "
                           "(%3.0f bytes/instance) %2.0f%% of object_bytes\n",
                           descriptor.c_str(), bytes, count,
                           average, percent);

        object_bytes_total += bytes;
      }
      os << "\n";
      os << std::flush;
      CHECK_EQ(object_bytes, object_bytes_total);

      os << StringPrintf("\tmanaged_code_bytes           = %8d (%2.0f%% of object_bytes)\n",
                         managed_code_bytes, PercentOfObjectBytes(managed_code_bytes));
      os << StringPrintf("\tmanaged_to_native_code_bytes = %8d (%2.0f%% of object_bytes)\n",
                         managed_to_native_code_bytes,
                         PercentOfObjectBytes(managed_to_native_code_bytes));
      os << StringPrintf("\tnative_to_managed_code_bytes = %8d (%2.0f%% of object_bytes)\n",
                         native_to_managed_code_bytes,
                         PercentOfObjectBytes(native_to_managed_code_bytes));
      os << "\n";
      os << std::flush;

      os << StringPrintf("\tregister_map_bytes     = %7d (%2.0f%% of object_bytes)\n",
                         register_map_bytes, PercentOfObjectBytes(register_map_bytes));
      os << StringPrintf("\tpc_mapping_table_bytes = %7d (%2.0f%% of object_bytes)\n",
                         pc_mapping_table_bytes, PercentOfObjectBytes(pc_mapping_table_bytes));
      os << "\n";
      os << std::flush;

      os << StringPrintf("\tdex_instruction_bytes = %d\n", dex_instruction_bytes);
      os << StringPrintf("\tmanaged_code_bytes expansion = %.2f\n",
                         static_cast<double>(managed_code_bytes)
                         / static_cast<double>(dex_instruction_bytes));
      os << "\n";
      os << std::flush;

    }
  } stats_;

 private:
  const Space& dump_space_;
  std::ostream& os_;

  DISALLOW_COPY_AND_ASSIGN(OatDump);
};

int oatdump(int argc, char** argv) {
  // Skip over argv[0].
  argv++;
  argc--;

  if (argc == 0) {
    fprintf(stderr, "no arguments specified\n");
    usage();
  }

  const char* image_filename = NULL;
  const char* boot_image_filename = NULL;
  std::string host_prefix;
  std::ostream* os = &std::cout;
  UniquePtr<std::ofstream> out;

  for (int i = 0; i < argc; i++) {
    const StringPiece option(argv[i]);
    if (option.starts_with("--image=")) {
      image_filename = option.substr(strlen("--image=")).data();
    } else if (option.starts_with("--boot-image=")) {
      boot_image_filename = option.substr(strlen("--boot-image=")).data();
    } else if (option.starts_with("--host-prefix=")) {
      host_prefix = option.substr(strlen("--host-prefix=")).data();
    } else if (option.starts_with("--output=")) {
      const char* filename = option.substr(strlen("--output=")).data();
      out.reset(new std::ofstream(filename));
      if (!out->good()) {
        fprintf(stderr, "failed to open output filename %s\n", filename);
        usage();
      }
      os = out.get();
    } else {
      fprintf(stderr, "unknown argument %s\n", option.data());
      usage();
    }
  }

  if (image_filename == NULL) {
   fprintf(stderr, "--image file name not specified\n");
   return EXIT_FAILURE;
  }

  Runtime::Options options;
  std::string image_option;
  std::string oat_option;
  std::string boot_image_option;
  std::string boot_oat_option;
  if (boot_image_filename != NULL) {
    boot_image_option += "-Ximage:";
    boot_image_option += boot_image_filename;
    options.push_back(std::make_pair(boot_image_option.c_str(), reinterpret_cast<void*>(NULL)));
  }
  image_option += "-Ximage:";
  image_option += image_filename;
  options.push_back(std::make_pair(image_option.c_str(), reinterpret_cast<void*>(NULL)));

  if (!host_prefix.empty()) {
    options.push_back(std::make_pair("host-prefix", host_prefix.c_str()));
  }

  UniquePtr<Runtime> runtime(Runtime::Create(options, false));
  if (runtime.get() == NULL) {
    fprintf(stderr, "could not create runtime\n");
    return EXIT_FAILURE;
  }

  Space* image_space = Heap::GetSpaces()[Heap::GetSpaces().size()-2];
  CHECK(image_space != NULL);
  const ImageHeader& image_header = image_space->GetImageHeader();
  if (!image_header.IsValid()) {
    fprintf(stderr, "invalid image header %s\n", image_filename);
    return EXIT_FAILURE;
  }
  OatDump::Dump(image_filename, *os, *image_space, image_header);
  return EXIT_SUCCESS;
}

} // namespace art

int main(int argc, char** argv) {
  return art::oatdump(argc, argv);
}
