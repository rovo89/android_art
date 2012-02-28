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

#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "class_linker.h"
#include "file.h"
#include "image.h"
#include "object_utils.h"
#include "os.h"
#include "runtime.h"
#include "space.h"
#include "stringpiece.h"

namespace art {

static void usage() {
  fprintf(stderr,
          "Usage: oatdump [options] ...\n"
          "    Example: oatdump --image=$ANDROID_PRODUCT_OUT/system/framework/boot.art --host-prefix=$ANDROID_PRODUCT_OUT\n"
          "    Example: adb shell oatdump --image=/system/framework/boot.art\n"
          "\n");
  fprintf(stderr,
          "  --oat-file=<file.oat>: specifies an input oat filename.\n"
          "      Example: --image=/system/framework/boot.oat\n"
          "\n");
  fprintf(stderr,
          "  --image=<file.art>: specifies an input image filename.\n"
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
  "kInstanceResolutionStubArray",
  "kStaticResolutionStubArray",
  "kUnknownMethodResolutionStubArray",
  "kResolutionMethod",
  "kCalleeSaveMethod",
  "kRefsOnlySaveMethod",
  "kRefsAndArgsSaveMethod",
  "kOatLocation",
  "kDexCaches",
  "kClassRoots",
};

class OatDump {
 public:
  static void Dump(const std::string& oat_filename,
                   std::ostream& os,
                   const OatFile& oat_file) {
    const OatHeader& oat_header = oat_file.GetOatHeader();

    os << "MAGIC:\n";
    os << oat_header.GetMagic() << "\n\n";

    os << "CHECKSUM:\n";
    os << StringPrintf("%08x\n\n", oat_header.GetChecksum());

    os << "DEX FILE COUNT:\n";
    os << oat_header.GetDexFileCount() << "\n\n";

    os << "EXECUTABLE OFFSET:\n";
    os << StringPrintf("%08x\n\n", oat_header.GetExecutableOffset());

    os << "BEGIN:\n";
    os << reinterpret_cast<const void*>(oat_file.Begin()) << "\n\n";

    os << "END:\n";
    os << reinterpret_cast<const void*>(oat_file.End()) << "\n\n";

    os << std::flush;

    std::vector<const OatFile::OatDexFile*> oat_dex_files = oat_file.GetOatDexFiles();
    for (size_t i = 0; i < oat_dex_files.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files[i];
      CHECK(oat_dex_file != NULL);
      DumpOatDexFile(os, oat_file, *oat_dex_file);
    }
  }

 private:
  static void DumpOatDexFile(std::ostream& os,
                             const OatFile& oat_file,
                             const OatFile::OatDexFile& oat_dex_file) {
    os << "OAT DEX FILE:\n";
    os << StringPrintf("location: %s\n", oat_dex_file.GetDexFileLocation().c_str());
    os << StringPrintf("checksum: %08x\n", oat_dex_file.GetDexFileLocationChecksum());
    UniquePtr<const DexFile> dex_file(oat_dex_file.OpenDexFile());
    if (dex_file.get() == NULL) {
      os << "NOT FOUND\n\n";
      return;
    }
    for (size_t class_def_index = 0; class_def_index < dex_file->NumClassDefs(); class_def_index++) {
      const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
      const char* descriptor = dex_file->GetClassDescriptor(class_def);
      UniquePtr<const OatFile::OatClass> oat_class(oat_dex_file.GetOatClass(class_def_index));
      CHECK(oat_class.get() != NULL);
      os << StringPrintf("%zd: %s (type_idx=%d) (", class_def_index, descriptor, class_def.class_idx_)
         << oat_class->GetStatus() << ")\n";
      DumpOatClass(os, oat_file, *oat_class.get(), *(dex_file.get()), class_def);
    }

    os << std::flush;
  }

  static void DumpOatClass(std::ostream& os,
                           const OatFile& oat_file,
                           const OatFile::OatClass& oat_class,
                           const DexFile& dex_file,
                           const DexFile::ClassDef& class_def) {
    const byte* class_data = dex_file.GetClassData(class_def);
    if (class_data == NULL) {  // empty class such as a marker interface?
      return;
    }
    ClassDataItemIterator it(dex_file, class_data);

    // just skipping through the fields to advance class_data
    while (it.HasNextStaticField()) {
      it.Next();
    }
    while (it.HasNextInstanceField()) {
      it.Next();
    }

    uint32_t method_index = 0;
    while (it.HasNextDirectMethod()) {
      const OatFile::OatMethod oat_method = oat_class.GetOatMethod(method_index);
      DumpOatMethod(os, method_index, oat_file, oat_method, dex_file, it.GetMemberIndex());
      method_index++;
      it.Next();
    }
    while (it.HasNextVirtualMethod()) {
      const OatFile::OatMethod oat_method = oat_class.GetOatMethod(method_index);
      DumpOatMethod(os, method_index, oat_file, oat_method, dex_file, it.GetMemberIndex());
      method_index++;
      it.Next();
    }
    DCHECK(!it.HasNext());
    os << std::flush;
  }
  static void DumpOatMethod(std::ostream& os,
                            uint32_t method_index,
                            const OatFile& oat_file,
                            const OatFile::OatMethod& oat_method,
                            const DexFile& dex_file,
                            uint32_t method_idx) {
    const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
    const char* name = dex_file.GetMethodName(method_id);
    std::string signature(dex_file.GetMethodSignature(method_id));
    os << StringPrintf("\t%d: %s %s (method_idx=%d)\n",
                       method_index, name, signature.c_str(), method_idx);
    os << StringPrintf("\t\tcode: %p (offset=%08x)\n",
                       oat_method.GetCode(), oat_method.GetCodeOffset());
    os << StringPrintf("\t\tframe_size_in_bytes: %zd\n",
                       oat_method.GetFrameSizeInBytes());
    os << StringPrintf("\t\tcore_spill_mask: %08x\n",
                       oat_method.GetCoreSpillMask());
    os << StringPrintf("\t\tfp_spill_mask: %08x\n",
                       oat_method.GetFpSpillMask());
    os << StringPrintf("\t\tmapping_table: %p (offset=%08x)\n",
                       oat_method.GetMappingTable(), oat_method.GetMappingTableOffset());
    os << StringPrintf("\t\tvmap_table: %p (offset=%08x)\n",
                       oat_method.GetVmapTable(), oat_method.GetVmapTableOffset());
    os << StringPrintf("\t\tgc_map: %p (offset=%08x)\n",
                       oat_method.GetGcMap(), oat_method.GetGcMapOffset());
    os << StringPrintf("\t\tinvoke_stub: %p (offset=%08x)\n",
                       oat_method.GetInvokeStub(), oat_method.GetInvokeStubOffset());
  }
};

class ImageDump {
 public:
  static void Dump(const std::string& image_filename,
                   const std::string& host_prefix,
                   std::ostream& os,
                   Space& image_space,
                   const ImageHeader& image_header) {
    os << "MAGIC:\n";
    os << image_header.GetMagic() << "\n\n";

    os << "IMAGE BEGIN:\n";
    os << reinterpret_cast<void*>(image_header.GetImageBegin()) << "\n\n";

    os << "OAT CHECKSUM:\n";
    os << StringPrintf("%08x\n\n", image_header.GetOatChecksum());

    os << "OAT BEGIN:\n";
    os << reinterpret_cast<void*>(image_header.GetOatBegin()) << "\n\n";

    os << "OAT END:\n";
    os << reinterpret_cast<void*>(image_header.GetOatEnd()) << "\n\n";

    os << "ROOTS:\n";
    os << reinterpret_cast<void*>(image_header.GetImageRoots()) << "\n";
    CHECK_EQ(arraysize(image_roots_descriptions_), size_t(ImageHeader::kImageRootsMax));
    for (int i = 0; i < ImageHeader::kImageRootsMax; i++) {
      ImageHeader::ImageRoot image_root = static_cast<ImageHeader::ImageRoot>(i);
      const char* image_root_description = image_roots_descriptions_[i];
      Object* image_root_object = image_header.GetImageRoot(image_root);
      os << StringPrintf("%s: %p\n", image_root_description, image_root_object);
      if (image_root_object->IsObjectArray()) {
        // TODO: replace down_cast with AsObjectArray (g++ currently has a problem with this)
        ObjectArray<Object>* image_root_object_array
            = down_cast<ObjectArray<Object>*>(image_root_object);
        //  = image_root_object->AsObjectArray<Object>();
        for (int i = 0; i < image_root_object_array->GetLength(); i++) {
          Object* value = image_root_object_array->Get(i);
          if (value != NULL) {
            os << "\t" << i << ": ";
            std::string summary;
            PrettyObjectValue(summary, value->GetClass(), value);
            os << summary;
          } else {
            os << StringPrintf("\t%d: null\n", i);
          }
        }
      }
    }
    os << "\n";

    os << "OBJECTS:\n" << std::flush;
    ImageDump state(image_space, os);
    HeapBitmap* heap_bitmap = Heap::GetLiveBits();
    DCHECK(heap_bitmap != NULL);
    heap_bitmap->Walk(ImageDump::Callback, &state);
    os << "\n";

    os << "STATS:\n" << std::flush;
    UniquePtr<File> file(OS::OpenFile(image_filename.c_str(), false));
    state.stats_.file_bytes = file->Length();
    size_t header_bytes = sizeof(ImageHeader);
    state.stats_.header_bytes = header_bytes;
    size_t alignment_bytes = RoundUp(header_bytes, kObjectAlignment) - header_bytes;
    state.stats_.alignment_bytes += alignment_bytes;
    state.stats_.Dump(os);
    os << "\n";

    os << std::flush;

    os << "OAT LOCATION:\n" << std::flush;
    Object* oat_location_object = image_header.GetImageRoot(ImageHeader::kOatLocation);
    std::string oat_location(oat_location_object->AsString()->ToModifiedUtf8());
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    os << oat_location;
    if (!host_prefix.empty()) {
      oat_location = host_prefix + oat_location;
      os << " (" << oat_location << ")";
    }
    os << "\n";
    const OatFile* oat_file = class_linker->FindOatFileFromOatLocation(oat_location);
    if (oat_file == NULL) {
      os << "NOT FOUND\n";
      os << std::flush;
      return;
    }
    os << "\n";
    os << std::flush;

    OatDump::Dump(oat_location, os, *oat_file);
  }

 private:

  ImageDump(const Space& dump_space, std::ostream& os) : dump_space_(dump_space), os_(os) {}

  ~ImageDump() {}

  static void PrettyObjectValue(std::string& summary, Class* type, Object* value) {
    CHECK(type != NULL);
    if (value == NULL) {
      StringAppendF(&summary, "null   %s\n", PrettyDescriptor(type).c_str());
    } else if (type->IsStringClass()) {
      String* string = value->AsString();
      StringAppendF(&summary, "%p   String: \"%s\"\n", string, string->ToModifiedUtf8().c_str());
    } else if (value->IsClass()) {
      Class* klass = value->AsClass();
      StringAppendF(&summary, "%p   Class: %s\n", klass, PrettyDescriptor(klass).c_str());
    } else if (value->IsField()) {
      Field* field = value->AsField();
      StringAppendF(&summary, "%p   Field: %s\n", field, PrettyField(field).c_str());
    } else if (value->IsMethod()) {
      Method* method = value->AsMethod();
      StringAppendF(&summary, "%p   Method: %s\n", method, PrettyMethod(method).c_str());
    } else {
      StringAppendF(&summary, "%p   %s\n", value, PrettyDescriptor(type).c_str());
    }
  }

  static void PrintField(std::string& summary, Field* field, Object* obj) {
    FieldHelper fh(field);
    Class* type = fh.GetType();
    StringAppendF(&summary, "\t%s: ", fh.GetName());
    if (type->IsPrimitiveLong()) {
      StringAppendF(&summary, "%lld (0x%llx)\n", field->Get64(obj), field->Get64(obj));
    } else if (type->IsPrimitiveDouble()) {
      StringAppendF(&summary, "%f (%a)\n", field->GetDouble(obj), field->GetDouble(obj));
    } else if (type->IsPrimitiveFloat()) {
      StringAppendF(&summary, "%f (%a)\n", field->GetFloat(obj), field->GetFloat(obj));
    } else if (type->IsPrimitive()){
      StringAppendF(&summary, "%d (0x%x)\n", field->Get32(obj), field->Get32(obj));
    } else {
      Object* value = field->GetObj(obj);
      PrettyObjectValue(summary, type, value);
    }
  }

  static void DumpFields(std::string& summary, Object* obj, Class* klass) {
    Class* super = klass->GetSuperClass();
    if (super != NULL) {
      DumpFields(summary, obj, super);
    }
    ObjectArray<Field>* fields = klass->GetIFields();
    if (fields != NULL) {
      for (int32_t i = 0; i < fields->GetLength(); i++) {
        Field* field = fields->Get(i);
        PrintField(summary, field, obj);
      }
    }
  }

  static void Callback(Object* obj, void* arg) {
    DCHECK(obj != NULL);
    DCHECK(arg != NULL);
    ImageDump* state = reinterpret_cast<ImageDump*>(arg);
    if (!state->InDumpSpace(obj)) {
      return;
    }

    size_t object_bytes = obj->SizeOf();
    size_t alignment_bytes = RoundUp(object_bytes, kObjectAlignment) - object_bytes;
    state->stats_.object_bytes += object_bytes;
    state->stats_.alignment_bytes += alignment_bytes;

    std::string summary;
    Class* obj_class = obj->GetClass();
    if (obj_class->IsArrayClass()) {
      StringAppendF(&summary, "%p: %s length:%d\n", obj, PrettyDescriptor(obj_class).c_str(),
                    obj->AsArray()->GetLength());
    } else if (obj->IsClass()) {
      Class* klass = obj->AsClass();
      StringAppendF(&summary, "%p: java.lang.Class \"%s\" (", obj,
                    PrettyDescriptor(klass).c_str());
      std::ostringstream ss;
      ss << klass->GetStatus() << ")\n";
      summary += ss.str();
    } else if (obj->IsField()) {
      StringAppendF(&summary, "%p: java.lang.reflect.Field %s\n", obj,
                    PrettyField(obj->AsField()).c_str());
    } else if (obj->IsMethod()) {
      StringAppendF(&summary, "%p: java.lang.reflect.Method %s\n", obj,
                    PrettyMethod(obj->AsMethod()).c_str());
    } else if (obj_class->IsStringClass()) {
      StringAppendF(&summary, "%p: java.lang.String \"%s\"\n", obj,
                    obj->AsString()->ToModifiedUtf8().c_str());
    } else {
      StringAppendF(&summary, "%p: %s\n", obj, PrettyDescriptor(obj_class).c_str());
    }
    DumpFields(summary, obj, obj_class);
    if (obj->IsObjectArray()) {
      ObjectArray<Object>* obj_array = obj->AsObjectArray<Object>();
      int32_t length = obj_array->GetLength();
      for (int32_t i = 0; i < length; i++) {
        Object* value = obj_array->Get(i);
        size_t run = 0;
        for (int32_t j = i + 1; j < length; j++) {
          if (value == obj_array->Get(j)) {
            run++;
          } else {
            break;
          }
        }
        if (run == 0) {
          StringAppendF(&summary, "\t%d: ", i);
        } else {
          StringAppendF(&summary, "\t%d to %zd: ", i, i + run);
          i = i + run;
        }
        Class* value_class = value == NULL ? obj_class->GetComponentType() : value->GetClass();
        PrettyObjectValue(summary, value_class, value);
      }
    } else if (obj->IsClass()) {
      ObjectArray<Field>* sfields = obj->AsClass()->GetSFields();
      if (sfields != NULL) {
        summary += "\t\tSTATICS:\n";
        for (int32_t i = 0; i < sfields->GetLength(); i++) {
          Field* field = sfields->Get(i);
          PrintField(summary, field, NULL);
        }
      }
    } else if (obj->IsMethod()) {
      Method* method = obj->AsMethod();
      if (method->IsNative()) {
        DCHECK(method->GetGcMap() == NULL) << PrettyMethod(method);
        DCHECK_EQ(0U, method->GetGcMapLength()) << PrettyMethod(method);
        DCHECK(method->GetMappingTable() == NULL) << PrettyMethod(method);
      } else if (method->IsAbstract() || method->IsCalleeSaveMethod() ||
          method->IsResolutionMethod()) {
        DCHECK(method->GetGcMap() == NULL) << PrettyMethod(method);
        DCHECK_EQ(0U, method->GetGcMapLength()) << PrettyMethod(method);
        DCHECK(method->GetMappingTable() == NULL) << PrettyMethod(method);
      } else {
        DCHECK(method->GetGcMap() != NULL) << PrettyMethod(method);
        DCHECK_NE(0U, method->GetGcMapLength()) << PrettyMethod(method);

        size_t register_map_bytes = method->GetGcMapLength();
        state->stats_.register_map_bytes += register_map_bytes;

        size_t pc_mapping_table_bytes = method->GetMappingTableLength();
        state->stats_.pc_mapping_table_bytes += pc_mapping_table_bytes;

        const DexFile::CodeItem* code_item = MethodHelper(method).GetCodeItem();
        size_t dex_instruction_bytes = code_item->insns_size_in_code_units_ * 2;
        state->stats_.dex_instruction_bytes += dex_instruction_bytes;

        StringAppendF(&summary, "\t\tSIZE: Dex Instructions=%zd GC=%zd Mapping=%zd\n",
                      dex_instruction_bytes, register_map_bytes, pc_mapping_table_bytes);
      }
    }
    std::string descriptor(ClassHelper(obj_class).GetDescriptor());
    state->stats_.descriptor_to_bytes[descriptor] += object_bytes;
    state->stats_.descriptor_to_count[descriptor] += 1;

    state->os_ << summary << std::flush;
  }

  bool InDumpSpace(const Object* object) {
    return dump_space_.Contains(object);
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

    typedef std::map<std::string, size_t> TableBytes;
    TableBytes descriptor_to_bytes;

    typedef std::map<std::string, size_t> TableCount;
    TableCount descriptor_to_count;

    double PercentOfFileBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(file_bytes)) * 100;
    }

    double PercentOfObjectBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(object_bytes)) * 100;
    }

    void Dump(std::ostream& os) {
      os << StringPrintf("\tfile_bytes = %zd\n", file_bytes);
      os << "\n";

      os << "\tfile_bytes = header_bytes + object_bytes + alignment_bytes\n";
      os << StringPrintf("\theader_bytes    = %10zd (%2.0f%% of file_bytes)\n",
                         header_bytes, PercentOfFileBytes(header_bytes));
      os << StringPrintf("\tobject_bytes    = %10zd (%2.0f%% of file_bytes)\n",
                         object_bytes, PercentOfFileBytes(object_bytes));
      os << StringPrintf("\talignment_bytes = %10zd (%2.0f%% of file_bytes)\n",
                         alignment_bytes, PercentOfFileBytes(alignment_bytes));
      os << "\n";
      os << std::flush;
      CHECK_EQ(file_bytes, header_bytes + object_bytes + alignment_bytes);

      os << "\tobject_bytes = sum of descriptor_to_bytes values below:\n";
      size_t object_bytes_total = 0;
      typedef TableBytes::const_iterator It;  // TODO: C++0x auto
      for (It it = descriptor_to_bytes.begin(), end = descriptor_to_bytes.end(); it != end; ++it) {
        const std::string& descriptor(it->first);
        size_t bytes = it->second;
        size_t count = descriptor_to_count[descriptor];
        double average = static_cast<double>(bytes) / static_cast<double>(count);
        double percent = PercentOfObjectBytes(bytes);
        os << StringPrintf("\t%32s %8zd bytes %6zd instances "
                           "(%3.0f bytes/instance) %2.0f%% of object_bytes\n",
                           descriptor.c_str(), bytes, count,
                           average, percent);

        object_bytes_total += bytes;
      }
      os << "\n";
      os << std::flush;
      CHECK_EQ(object_bytes, object_bytes_total);

      os << StringPrintf("\tmanaged_code_bytes           = %8zd (%2.0f%% of object_bytes)\n",
                         managed_code_bytes, PercentOfObjectBytes(managed_code_bytes));
      os << StringPrintf("\tmanaged_to_native_code_bytes = %8zd (%2.0f%% of object_bytes)\n",
                         managed_to_native_code_bytes,
                         PercentOfObjectBytes(managed_to_native_code_bytes));
      os << StringPrintf("\tnative_to_managed_code_bytes = %8zd (%2.0f%% of object_bytes)\n",
                         native_to_managed_code_bytes,
                         PercentOfObjectBytes(native_to_managed_code_bytes));
      os << "\n";
      os << std::flush;

      os << StringPrintf("\tregister_map_bytes     = %7zd (%2.0f%% of object_bytes)\n",
                         register_map_bytes, PercentOfObjectBytes(register_map_bytes));
      os << StringPrintf("\tpc_mapping_table_bytes = %7zd (%2.0f%% of object_bytes)\n",
                         pc_mapping_table_bytes, PercentOfObjectBytes(pc_mapping_table_bytes));
      os << "\n";
      os << std::flush;

      os << StringPrintf("\tdex_instruction_bytes = %zd\n", dex_instruction_bytes);
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

  DISALLOW_COPY_AND_ASSIGN(ImageDump);
};

int oatdump(int argc, char** argv) {
  // Skip over argv[0].
  argv++;
  argc--;

  if (argc == 0) {
    fprintf(stderr, "No arguments specified\n");
    usage();
  }

  const char* oat_filename = NULL;
  const char* image_filename = NULL;
  const char* boot_image_filename = NULL;
  std::string host_prefix;
  std::ostream* os = &std::cout;
  UniquePtr<std::ofstream> out;

  for (int i = 0; i < argc; i++) {
    const StringPiece option(argv[i]);
    if (option.starts_with("--oat-file=")) {
      oat_filename = option.substr(strlen("--oat-file=")).data();
    } else if (option.starts_with("--image=")) {
      image_filename = option.substr(strlen("--image=")).data();
    } else if (option.starts_with("--boot-image=")) {
      boot_image_filename = option.substr(strlen("--boot-image=")).data();
    } else if (option.starts_with("--host-prefix=")) {
      host_prefix = option.substr(strlen("--host-prefix=")).data();
    } else if (option.starts_with("--output=")) {
      const char* filename = option.substr(strlen("--output=")).data();
      out.reset(new std::ofstream(filename));
      if (!out->good()) {
        fprintf(stderr, "Failed to open output filename %s\n", filename);
        usage();
      }
      os = out.get();
    } else {
      fprintf(stderr, "Unknown argument %s\n", option.data());
      usage();
    }
  }

  if (image_filename == NULL && oat_filename == NULL) {
    fprintf(stderr, "Either --image or --oat must be specified\n");
    return EXIT_FAILURE;
  }

  if (image_filename != NULL && oat_filename != NULL) {
    fprintf(stderr, "Either --image or --oat must be specified but not both\n");
    return EXIT_FAILURE;
  }

  if (oat_filename != NULL) {
    const OatFile* oat_file = OatFile::Open(oat_filename, oat_filename, NULL);
    if (oat_file == NULL) {
      fprintf(stderr, "Failed to open oat file from %s\n", oat_filename);
      return EXIT_FAILURE;
    }
    OatDump::Dump(oat_filename, *os, *oat_file);
    return EXIT_SUCCESS;
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
  if (image_filename != NULL) {
    image_option += "-Ximage:";
    image_option += image_filename;
    options.push_back(std::make_pair(image_option.c_str(), reinterpret_cast<void*>(NULL)));
  }

  if (!host_prefix.empty()) {
    options.push_back(std::make_pair("host-prefix", host_prefix.c_str()));
  }

  UniquePtr<Runtime> runtime(Runtime::Create(options, false));
  if (runtime.get() == NULL) {
    fprintf(stderr, "Failed to create runtime\n");
    return EXIT_FAILURE;
  }

  ImageSpace* image_space = Heap::GetSpaces()[Heap::GetSpaces().size()-2]->AsImageSpace();
  CHECK(image_space != NULL);
  const ImageHeader& image_header = image_space->GetImageHeader();
  if (!image_header.IsValid()) {
    fprintf(stderr, "Invalid image header %s\n", image_filename);
    return EXIT_FAILURE;
  }
  ImageDump::Dump(image_filename, host_prefix, *os, *image_space, image_header);
  return EXIT_SUCCESS;
}

} // namespace art

int main(int argc, char** argv) {
  return art::oatdump(argc, argv);
}
