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
#include "context.h"
#include "dex_instruction.h"
#include "dex_verifier.h"
#include "disassembler.h"
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
          "  --extract-elf-to=<file.elf>: provide the prefix of the filename for\n"
          "      the output ELF files.\n"
          "      Example: --extract-elf-to=output.elf\n"
          "\n");
  fprintf(stderr,
          "  --host-prefix may be used to translate host paths to target paths during\n"
          "      cross compilation.\n"
          "      Example: --host-prefix=out/target/product/crespo\n"
          "      Default: $ANDROID_PRODUCT_OUT\n"
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

class OatDumper {
 public:
  explicit OatDumper(const std::string& host_prefix, const OatFile& oat_file)
    : host_prefix_(host_prefix),
      oat_file_(oat_file),
      oat_dex_files_(oat_file.GetOatDexFiles()),
      disassembler_(Disassembler::Create(oat_file_.GetOatHeader().GetInstructionSet())) {
    AddAllOffsets();
  }

  void Dump(std::ostream& os) {
    const OatHeader& oat_header = oat_file_.GetOatHeader();

    os << "MAGIC:\n";
    os << oat_header.GetMagic() << "\n\n";

    os << "CHECKSUM:\n";
    os << StringPrintf("0x%08x\n\n", oat_header.GetChecksum());

    os << "INSTRUCTION SET:\n";
    os << oat_header.GetInstructionSet() << "\n\n";

    os << "DEX FILE COUNT:\n";
    os << oat_header.GetDexFileCount() << "\n\n";

    os << "ELF IMAGE COUNT:\n";
    os << oat_header.GetElfImageCount() << "\n\n";

    os << "EXECUTABLE OFFSET:\n";
    os << StringPrintf("0x%08x\n\n", oat_header.GetExecutableOffset());

    os << "IMAGE FILE LOCATION CHECKSUM:\n";
    os << StringPrintf("0x%08x\n\n", oat_header.GetImageFileLocationChecksum());

    os << "IMAGE FILE LOCATION:\n";
    const std::string image_file_location(oat_header.GetImageFileLocation());
    os << image_file_location;
    if (!image_file_location.empty() && !host_prefix_.empty()) {
      os << " (" << host_prefix_ << image_file_location << ")";
    }
    os << "\n\n";

    os << "BEGIN:\n";
    os << reinterpret_cast<const void*>(oat_file_.Begin()) << "\n\n";

    os << "END:\n";
    os << reinterpret_cast<const void*>(oat_file_.End()) << "\n\n";

    os << std::flush;

    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != NULL);
      DumpOatDexFile(os, *oat_dex_file);
    }
  }

  size_t ComputeSize(const void* oat_data) {
    if (reinterpret_cast<const byte*>(oat_data) < oat_file_.Begin() ||
        reinterpret_cast<const byte*>(oat_data) > oat_file_.End()) {
      return 0;  // Address not in oat file
    }
    uint32_t begin_offset = reinterpret_cast<size_t>(oat_data) -
                            reinterpret_cast<size_t>(oat_file_.Begin());
    typedef std::set<uint32_t>::iterator It;
    It it = offsets_.upper_bound(begin_offset);
    CHECK(it != offsets_.end());
    uint32_t end_offset = *it;
    return end_offset - begin_offset;
  }

  InstructionSet GetInstructionSet() {
    return oat_file_.GetOatHeader().GetInstructionSet();
  }

  const void* GetOatCode(Method* m) {
    MethodHelper mh(m);
    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != NULL);
      UniquePtr<const DexFile> dex_file(oat_dex_file->OpenDexFile());
      if (dex_file.get() != NULL) {
        uint32_t class_def_index;
        bool found = dex_file->FindClassDefIndex(mh.GetDeclaringClassDescriptor(), class_def_index);
        if (found) {
          const OatFile::OatClass* oat_class = oat_dex_file->GetOatClass(class_def_index);
          CHECK(oat_class != NULL);
          size_t method_index = m->GetMethodIndex();
          return oat_class->GetOatMethod(method_index).GetCode();
        }
      }
    }
    return NULL;
  }

 private:
  void AddAllOffsets() {
    // We don't know the length of the code for each method, but we need to know where to stop
    // when disassembling. What we do know is that a region of code will be followed by some other
    // region, so if we keep a sorted sequence of the start of each region, we can infer the length
    // of a piece of code by using upper_bound to find the start of the next region.
    for (size_t i = 0; i < oat_dex_files_.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files_[i];
      CHECK(oat_dex_file != NULL);
      UniquePtr<const DexFile> dex_file(oat_dex_file->OpenDexFile());
      if (dex_file.get() == NULL) {
        continue;
      }
      offsets_.insert(reinterpret_cast<uint32_t>(&dex_file->GetHeader()));
      for (size_t class_def_index = 0; class_def_index < dex_file->NumClassDefs(); class_def_index++) {
        const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
        UniquePtr<const OatFile::OatClass> oat_class(oat_dex_file->GetOatClass(class_def_index));
        const byte* class_data = dex_file->GetClassData(class_def);
        if (class_data != NULL) {
          ClassDataItemIterator it(*dex_file, class_data);
          SkipAllFields(it);
          uint32_t class_method_index = 0;
          while (it.HasNextDirectMethod()) {
            AddOffsets(oat_class->GetOatMethod(class_method_index++));
            it.Next();
          }
          while (it.HasNextVirtualMethod()) {
            AddOffsets(oat_class->GetOatMethod(class_method_index++));
            it.Next();
          }
        }
      }
    }

    // If the last thing in the file is code for a method, there won't be an offset for the "next"
    // thing. Instead of having a special case in the upper_bound code, let's just add an entry
    // for the end of the file.
    offsets_.insert(static_cast<uint32_t>(oat_file_.End() - oat_file_.Begin()));
  }

  void AddOffsets(const OatFile::OatMethod& oat_method) {
    uint32_t code_offset = oat_method.GetCodeOffset();
    if (oat_file_.GetOatHeader().GetInstructionSet() == kThumb2) {
      code_offset &= ~0x1;
    }
    offsets_.insert(code_offset);
    offsets_.insert(oat_method.GetMappingTableOffset());
    offsets_.insert(oat_method.GetVmapTableOffset());
    offsets_.insert(oat_method.GetGcMapOffset());
    offsets_.insert(oat_method.GetInvokeStubOffset());
  }

  void DumpOatDexFile(std::ostream& os, const OatFile::OatDexFile& oat_dex_file) {
    os << "OAT DEX FILE:\n";
    os << StringPrintf("location: %s\n", oat_dex_file.GetDexFileLocation().c_str());
    os << StringPrintf("checksum: 0x%08x\n", oat_dex_file.GetDexFileLocationChecksum());
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
      DumpOatClass(os, *oat_class.get(), *(dex_file.get()), class_def);
    }

    os << std::flush;
  }

  static void SkipAllFields(ClassDataItemIterator& it) {
    while (it.HasNextStaticField()) {
      it.Next();
    }
    while (it.HasNextInstanceField()) {
      it.Next();
    }
  }

  void DumpOatClass(std::ostream& os, const OatFile::OatClass& oat_class, const DexFile& dex_file,
                    const DexFile::ClassDef& class_def) {
    const byte* class_data = dex_file.GetClassData(class_def);
    if (class_data == NULL) {  // empty class such as a marker interface?
      return;
    }
    ClassDataItemIterator it(dex_file, class_data);
    SkipAllFields(it);

    uint32_t class_method_index = 0;
    while (it.HasNextDirectMethod()) {
      const OatFile::OatMethod oat_method = oat_class.GetOatMethod(class_method_index);
      DumpOatMethod(os, class_method_index, oat_method, dex_file,
                    it.GetMemberIndex(), it.GetMethodCodeItem());
      class_method_index++;
      it.Next();
    }
    while (it.HasNextVirtualMethod()) {
      const OatFile::OatMethod oat_method = oat_class.GetOatMethod(class_method_index);
      DumpOatMethod(os, class_method_index, oat_method, dex_file,
                    it.GetMemberIndex(), it.GetMethodCodeItem());
      class_method_index++;
      it.Next();
    }
    DCHECK(!it.HasNext());
    os << std::flush;
  }

  void DumpOatMethod(std::ostream& os, uint32_t class_method_index,
                     const OatFile::OatMethod& oat_method, const DexFile& dex_file,
                     uint32_t dex_method_idx, const DexFile::CodeItem* code_item) {
    const DexFile::MethodId& method_id = dex_file.GetMethodId(dex_method_idx);
    const char* name = dex_file.GetMethodName(method_id);
    std::string signature(dex_file.GetMethodSignature(method_id));
    os << StringPrintf("\t%d: %s %s (dex_method_idx=%d)\n",
                       class_method_index, name, signature.c_str(), dex_method_idx);
    os << StringPrintf("\t\tframe_size_in_bytes: %zd\n",
                       oat_method.GetFrameSizeInBytes());
    os << StringPrintf("\t\tcore_spill_mask: 0x%08x",
                       oat_method.GetCoreSpillMask());
    DumpSpillMask(os, oat_method.GetCoreSpillMask(), false);
    os << StringPrintf("\n\t\tfp_spill_mask: 0x%08x",
                       oat_method.GetFpSpillMask());
    DumpSpillMask(os, oat_method.GetFpSpillMask(), true);
    os << StringPrintf("\n\t\tmapping_table: %p (offset=0x%08x)\n",
                       oat_method.GetMappingTable(), oat_method.GetMappingTableOffset());
    DumpMappingTable(os, oat_method);
    os << StringPrintf("\t\tvmap_table: %p (offset=0x%08x)\n",
                       oat_method.GetVmapTable(), oat_method.GetVmapTableOffset());
    DumpVmap(os, oat_method.GetVmapTable(), oat_method.GetCoreSpillMask(),
             oat_method.GetFpSpillMask());
    os << StringPrintf("\t\tgc_map: %p (offset=0x%08x)\n",
                       oat_method.GetGcMap(), oat_method.GetGcMapOffset());
    DumpGcMap(os, oat_method.GetGcMap());
    os << StringPrintf("\t\tCODE: %p (offset=0x%08x size=%d)%s\n",
                       oat_method.GetCode(),
                       oat_method.GetCodeOffset(),
                       oat_method.GetCodeSize(),
                       oat_method.GetCode() != NULL ? "..." : "");
    DumpCode(os, oat_method.GetCode(), oat_method.GetMappingTable(), dex_file, code_item);
    os << StringPrintf("\t\tINVOKE STUB: %p (offset=0x%08x size=%d)%s\n",
                       oat_method.GetInvokeStub(),
                       oat_method.GetInvokeStubOffset(),
                       oat_method.GetInvokeStubSize(),
                       oat_method.GetInvokeStub() != NULL ? "..." : "");
    DumpCode(os, reinterpret_cast<const void*>(oat_method.GetInvokeStub()), NULL, dex_file, NULL);
  }

  void DumpSpillMask(std::ostream& os, uint32_t spill_mask, bool is_float) {
    if (spill_mask == 0) {
      return;
    }
    os << " (";
    for (size_t i = 0; i < 32; i++) {
      if ((spill_mask & (1 << i)) != 0) {
        if (is_float) {
          os << "fr" << i;
        } else {
          os << "r" << i;
        }
        spill_mask ^= 1 << i;  // clear bit
        if (spill_mask != 0) {
          os << ", ";
        } else {
          break;
        }
      }
    }
    os << ")";
  }

  void DumpVmap(std::ostream& os, const uint16_t* raw_table, uint32_t core_spill_mask,
                uint32_t fp_spill_mask) {
    if (raw_table == NULL) {
      return;
    }
    const VmapTable vmap_table(raw_table);
    bool first = true;
    os << "\t\t\t";
    for (size_t i = 0; i < vmap_table.size(); i++) {
      uint16_t dex_reg = vmap_table[i];
      size_t matches = 0;
      size_t spill_shifts = 0;
      uint32_t spill_mask = core_spill_mask;
      bool processing_fp = false;
      while (matches != (i + 1)) {
        if (spill_mask == 0) {
          CHECK(!processing_fp);
          spill_mask = fp_spill_mask;
          processing_fp = true;
        }
        matches += spill_mask & 1;  // Add 1 if the low bit is set
        spill_mask >>= 1;
        spill_shifts++;
      }
      size_t arm_reg = spill_shifts - 1;  // wind back one as we want the last match
      os << (first ? "v" : ", v")  << dex_reg;
      if (arm_reg < 16) {
        os << "/r" << arm_reg;
      } else {
        os << "/fr" << (arm_reg - 16);
      }
      if (first) {
        first = false;
      }
    }
    os << std::endl;
  }

  void DumpGcMap(std::ostream& os, const uint8_t* gc_map_raw) {
    if (gc_map_raw == NULL) {
      return;
    }
    uint32_t gc_map_length = (gc_map_raw[0] << 24) | (gc_map_raw[1] << 16) |
                             (gc_map_raw[2] << 8) | (gc_map_raw[3] << 0);
    verifier::PcToReferenceMap map(gc_map_raw + sizeof(uint32_t), gc_map_length);
    for (size_t entry = 0; entry < map.NumEntries(); entry++) {
      os << StringPrintf("\t\t\t0x%04x", map.GetPC(entry));
      size_t num_regs = map.RegWidth() * 8;
      const uint8_t* reg_bitmap = map.GetBitMap(entry);
      bool first = true;
      for (size_t reg = 0; reg < num_regs; reg++) {
        if (((reg_bitmap[reg / 8] >> (reg % 8)) & 0x01) != 0) {
          if (first) {
            os << "  v" << reg;
            first = false;
          } else {
            os << ", v" << reg;
          }
        }
      }
      os << std::endl;
    }
  }

  void DumpMappingTable(std::ostream& os, const OatFile::OatMethod& oat_method) {
    const uint32_t* raw_table = oat_method.GetMappingTable();
    const void* code = oat_method.GetCode();
    if (raw_table == NULL || code == NULL) {
      return;
    }

    uint32_t length = *raw_table;
    ++raw_table;

    os << "\t\t{";
    for (size_t i = 0; i < length; i += 2) {
      const uint8_t* native_pc = reinterpret_cast<const uint8_t*>(code) + raw_table[i];
      uint32_t dex_pc = raw_table[i + 1];
      os << StringPrintf("%p -> 0x%04x", native_pc, dex_pc);
      if (i + 2 < length) {
        os << ", ";
      }
    }
    os << "}" << std::endl << std::flush;
  }

  void DumpCode(std::ostream& os, const void* code, const uint32_t* raw_mapping_table,
                const DexFile& dex_file, const DexFile::CodeItem* code_item) {
    if (code == NULL) {
      return;
    }

    if (raw_mapping_table == NULL) {
      // code but no mapping table is most likely caused by code created by the JNI compiler
      const uint8_t* native_pc = reinterpret_cast<const uint8_t*>(code);
      const uint8_t* oat_begin = reinterpret_cast<const uint8_t*>(oat_file_.Begin());
      uint32_t last_offset = static_cast<uint32_t>(native_pc - oat_begin);

      typedef std::set<uint32_t>::iterator It;
      It it = offsets_.upper_bound(last_offset);
      CHECK(it != offsets_.end());
      const uint8_t* end_native_pc = reinterpret_cast<const uint8_t*>(oat_begin) + *it;
      CHECK(native_pc < end_native_pc);

      disassembler_->Dump(os, native_pc, end_native_pc);
      return;
    }

    uint32_t length = *raw_mapping_table;
    ++raw_mapping_table;

    for (size_t i = 0; i < length; i += 2) {
      uint32_t dex_pc = raw_mapping_table[i + 1];
      const Instruction* instruction = Instruction::At(&code_item->insns_[dex_pc]);
      os << StringPrintf("\t\t0x%04x: %s\n", dex_pc, instruction->DumpString(&dex_file).c_str());

      const uint8_t* native_pc = reinterpret_cast<const uint8_t*>(code) + raw_mapping_table[i];
      const uint8_t* end_native_pc = NULL;
      if (i + 2 < length) {
        end_native_pc = reinterpret_cast<const uint8_t*>(code) + raw_mapping_table[i + 2];
      } else {
        const uint8_t* oat_begin = reinterpret_cast<const uint8_t*>(oat_file_.Begin());
        uint32_t last_offset = static_cast<uint32_t>(native_pc - oat_begin);

        typedef std::set<uint32_t>::iterator It;
        It it = offsets_.upper_bound(last_offset);
        CHECK(it != offsets_.end());
        end_native_pc = reinterpret_cast<const uint8_t*>(oat_begin) + *it;
      }
      CHECK(native_pc < end_native_pc);
      disassembler_->Dump(os, native_pc, end_native_pc);
    }
  }

  const std::string host_prefix_;
  const OatFile& oat_file_;
  std::vector<const OatFile::OatDexFile*> oat_dex_files_;
  std::set<uint32_t> offsets_;
  UniquePtr<Disassembler> disassembler_;
};

class ImageDumper {
 public:
  explicit ImageDumper(std::ostream& os, const std::string& image_filename,
                       const std::string& host_prefix, Space& image_space,
                       const ImageHeader& image_header) : os_(os),
                       image_filename_(image_filename), host_prefix_(host_prefix),
                       image_space_(image_space), image_header_(image_header) {
  }

  void Dump() {
    os_ << "MAGIC:\n";
    os_ << image_header_.GetMagic() << "\n\n";

    os_ << "IMAGE BEGIN:\n";
    os_ << reinterpret_cast<void*>(image_header_.GetImageBegin()) << "\n\n";

    os_ << "OAT CHECKSUM:\n";
    os_ << StringPrintf("0x%08x\n\n", image_header_.GetOatChecksum());

    os_ << "OAT BEGIN:\n";
    os_ << reinterpret_cast<void*>(image_header_.GetOatBegin()) << "\n\n";

    os_ << "OAT END:\n";
    os_ << reinterpret_cast<void*>(image_header_.GetOatEnd()) << "\n\n";

    os_ << "ROOTS:\n";
    os_ << reinterpret_cast<void*>(image_header_.GetImageRoots()) << "\n";
    CHECK_EQ(arraysize(image_roots_descriptions_), size_t(ImageHeader::kImageRootsMax));
    for (int i = 0; i < ImageHeader::kImageRootsMax; i++) {
      ImageHeader::ImageRoot image_root = static_cast<ImageHeader::ImageRoot>(i);
      const char* image_root_description = image_roots_descriptions_[i];
      Object* image_root_object = image_header_.GetImageRoot(image_root);
      os_ << StringPrintf("%s: %p\n", image_root_description, image_root_object);
      if (image_root_object->IsObjectArray()) {
        // TODO: replace down_cast with AsObjectArray (g++ currently has a problem with this)
        ObjectArray<Object>* image_root_object_array
            = down_cast<ObjectArray<Object>*>(image_root_object);
        //  = image_root_object->AsObjectArray<Object>();
        for (int i = 0; i < image_root_object_array->GetLength(); i++) {
          Object* value = image_root_object_array->Get(i);
          if (value != NULL) {
            os_ << "\t" << i << ": ";
            std::string summary;
            PrettyObjectValue(summary, value->GetClass(), value);
            os_ << summary;
          } else {
            os_ << StringPrintf("\t%d: null\n", i);
          }
        }
      }
    }
    os_ << "\n";

    os_ << "OAT LOCATION:\n" << std::flush;
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    Object* oat_location_object = image_header_.GetImageRoot(ImageHeader::kOatLocation);
    std::string oat_location(oat_location_object->AsString()->ToModifiedUtf8());
    os_ << oat_location;
    if (!host_prefix_.empty()) {
      oat_location = host_prefix_ + oat_location;
      os_ << " (" << oat_location << ")";
    }
    os_ << "\n";
    const OatFile* oat_file = class_linker->FindOatFileFromOatLocation(oat_location);
    if (oat_file == NULL) {
      os_ << "NOT FOUND\n";
      return;
    }
    os_ << "\n";

    stats_.oat_file_bytes = oat_file->Size();

    oat_dumper_.reset(new OatDumper(host_prefix_, *oat_file));

    os_ << "OBJECTS:\n" << std::flush;
    HeapBitmap* heap_bitmap = Runtime::Current()->GetHeap()->GetLiveBits();
    DCHECK(heap_bitmap != NULL);
    heap_bitmap->Walk(ImageDumper::Callback, this);
    os_ << "\n";

    os_ << "STATS:\n" << std::flush;
    UniquePtr<File> file(OS::OpenFile(image_filename_.c_str(), false));
    stats_.file_bytes = file->Length();
    size_t header_bytes = sizeof(ImageHeader);
    stats_.header_bytes = header_bytes;
    size_t alignment_bytes = RoundUp(header_bytes, kObjectAlignment) - header_bytes;
    stats_.alignment_bytes += alignment_bytes;
    stats_.Dump(os_);
    os_ << "\n";

    os_ << std::flush;

    oat_dumper_->Dump(os_);
  }

 private:

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
    } else if (type->IsPrimitive()) {
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

  bool InDumpSpace(const Object* object) {
    return image_space_.Contains(object);
  }

  const void* GetOatCodeBegin(Method* m) {
    Runtime* runtime = Runtime::Current();
    const void* code = m->GetCode();
    if (code == runtime->GetResolutionStubArray(Runtime::kStaticMethod)->GetData()) {
      code = oat_dumper_->GetOatCode(m);
    }
    if (oat_dumper_->GetInstructionSet() == kThumb2) {
      code = reinterpret_cast<void*>(reinterpret_cast<uint32_t>(code) & ~0x1);
    }
    return code;
  }

  uint32_t GetOatCodeSize(Method* m) {
    const uint32_t* oat_code_begin = reinterpret_cast<const uint32_t*>(GetOatCodeBegin(m));
    if (oat_code_begin == NULL) {
      return 0;
    }
    return oat_code_begin[-1];
  }

  const void* GetOatCodeEnd(Method* m) {
    const uint8_t* oat_code_begin = reinterpret_cast<const uint8_t*>(GetOatCodeBegin(m));
    if (oat_code_begin == NULL) {
      return NULL;
    }
    return oat_code_begin + GetOatCodeSize(m);
  }

  static void Callback(Object* obj, void* arg) {
    DCHECK(obj != NULL);
    DCHECK(arg != NULL);
    ImageDumper* state = reinterpret_cast<ImageDumper*>(arg);
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
        bool first_occurrence;
        size_t invoke_stub_size = state->ComputeOatSize(
            reinterpret_cast<const void*>(method->GetInvokeStub()), &first_occurrence);
        if (first_occurrence) {
          state->stats_.managed_to_native_code_bytes += invoke_stub_size;
        }
        const void* oat_code = state->GetOatCodeBegin(method);
        size_t code_size = state->ComputeOatSize(oat_code, &first_occurrence);
        if (first_occurrence) {
          state->stats_.native_to_managed_code_bytes += code_size;
        }
        if (oat_code != method->GetCode()) {
          StringAppendF(&summary, "\t\tOAT CODE: %p\n", oat_code);
        }
      } else if (method->IsAbstract() || method->IsCalleeSaveMethod() ||
          method->IsResolutionMethod()) {
        DCHECK(method->GetGcMap() == NULL) << PrettyMethod(method);
        DCHECK_EQ(0U, method->GetGcMapLength()) << PrettyMethod(method);
        DCHECK(method->GetMappingTable() == NULL) << PrettyMethod(method);
      } else {
        DCHECK(method->GetGcMap() != NULL) << PrettyMethod(method);
        DCHECK_NE(0U, method->GetGcMapLength()) << PrettyMethod(method);

        const DexFile::CodeItem* code_item = MethodHelper(method).GetCodeItem();
        size_t dex_instruction_bytes = code_item->insns_size_in_code_units_ * 2;
        state->stats_.dex_instruction_bytes += dex_instruction_bytes;

        bool first_occurance;
        size_t gc_map_bytes = state->ComputeOatSize(method->GetGcMapRaw(), &first_occurance);
        if (first_occurance) {
          state->stats_.gc_map_bytes += gc_map_bytes;
        }

        size_t pc_mapping_table_bytes =
            state->ComputeOatSize(method->GetMappingTableRaw(), &first_occurance);
        if (first_occurance) {
          state->stats_.pc_mapping_table_bytes += pc_mapping_table_bytes;
        }

        size_t vmap_table_bytes =
            state->ComputeOatSize(method->GetVmapTableRaw(), &first_occurance);
        if (first_occurance) {
          state->stats_.vmap_table_bytes += vmap_table_bytes;
        }

        size_t invoke_stub_size = state->ComputeOatSize(
            reinterpret_cast<const void*>(method->GetInvokeStub()), &first_occurance);
        if (first_occurance) {
          state->stats_.native_to_managed_code_bytes += invoke_stub_size;
        }
        const void* oat_code_begin = state->GetOatCodeBegin(method);
        const void* oat_code_end = state->GetOatCodeEnd(method);
        // TODO: use oat_code_size and remove code_size based on offsets
        // uint32_t oat_code_size = state->GetOatCodeSize(method);
        size_t code_size = state->ComputeOatSize(oat_code_begin, &first_occurance);
        if (first_occurance) {
          state->stats_.managed_code_bytes += code_size;
        }
        state->stats_.managed_code_bytes_ignoring_deduplication += code_size;

        StringAppendF(&summary, "\t\tOAT CODE: %p-%p\n", oat_code_begin, oat_code_end);
        StringAppendF(&summary, "\t\tSIZE: Dex Instructions=%zd GC=%zd Mapping=%zd\n",
                      dex_instruction_bytes, gc_map_bytes, pc_mapping_table_bytes);

        size_t total_size = dex_instruction_bytes + gc_map_bytes + pc_mapping_table_bytes +
            vmap_table_bytes + invoke_stub_size + code_size + object_bytes;

        double expansion =
            static_cast<double>(code_size) / static_cast<double>(dex_instruction_bytes);
        state->stats_.ComputeOutliers(total_size, expansion, method);
      }
    }
    std::string descriptor(ClassHelper(obj_class).GetDescriptor());
    state->stats_.descriptor_to_bytes[descriptor] += object_bytes;
    state->stats_.descriptor_to_count[descriptor] += 1;

    state->os_ << summary << std::flush;
  }

  std::set<const void*> already_seen_;
  // Compute the size of the given data within the oat file and whether this is the first time
  // this data has been requested
  size_t ComputeOatSize(const void* oat_data, bool* first_occurance) {
    if (already_seen_.count(oat_data) == 0) {
      *first_occurance = true;
      already_seen_.insert(oat_data);
    } else {
      *first_occurance = false;
    }
    return oat_dumper_->ComputeSize(oat_data);
  }

 public:
  struct Stats {
    size_t oat_file_bytes;
    size_t file_bytes;

    size_t header_bytes;
    size_t object_bytes;
    size_t alignment_bytes;

    size_t managed_code_bytes;
    size_t managed_code_bytes_ignoring_deduplication;
    size_t managed_to_native_code_bytes;
    size_t native_to_managed_code_bytes;

    size_t gc_map_bytes;
    size_t pc_mapping_table_bytes;
    size_t vmap_table_bytes;

    size_t dex_instruction_bytes;

    std::vector<Method*> method_outlier;
    std::vector<size_t> method_outlier_size;
    std::vector<double> method_outlier_expansion;

    explicit Stats()
        : oat_file_bytes(0),
          file_bytes(0),
          header_bytes(0),
          object_bytes(0),
          alignment_bytes(0),
          managed_code_bytes(0),
          managed_code_bytes_ignoring_deduplication(0),
          managed_to_native_code_bytes(0),
          native_to_managed_code_bytes(0),
          gc_map_bytes(0),
          pc_mapping_table_bytes(0),
          vmap_table_bytes(0),
          dex_instruction_bytes(0) {}

    typedef std::map<std::string, size_t> TableBytes;
    TableBytes descriptor_to_bytes;

    typedef std::map<std::string, size_t> TableCount;
    TableCount descriptor_to_count;

    double PercentOfOatBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(oat_file_bytes)) * 100;
    }

    double PercentOfFileBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(file_bytes)) * 100;
    }

    double PercentOfObjectBytes(size_t size) {
      return (static_cast<double>(size) / static_cast<double>(object_bytes)) * 100;
    }

    void ComputeOutliers(size_t total_size, double expansion, Method* method) {
      method_outlier_size.push_back(total_size);
      method_outlier_expansion.push_back(expansion);
      method_outlier.push_back(method);
    }

    void DumpOutliers(std::ostream& os) {
      size_t sum_of_sizes = 0;
      size_t sum_of_sizes_squared = 0;
      size_t sum_of_expansion = 0;
      size_t sum_of_expansion_squared = 0;
      size_t n = method_outlier_size.size();
      for (size_t i = 0; i < n; i++) {
        size_t cur_size = method_outlier_size[i];
        sum_of_sizes += cur_size;
        sum_of_sizes_squared += cur_size * cur_size;
        double cur_expansion = method_outlier_expansion[i];
        sum_of_expansion += cur_expansion;
        sum_of_expansion_squared += cur_expansion * cur_expansion;
      }
      size_t size_mean = sum_of_sizes / n;
      size_t size_variance = (sum_of_sizes_squared - sum_of_sizes * size_mean) / (n - 1);
      double expansion_mean = sum_of_expansion / n;
      double expansion_variance =
          (sum_of_expansion_squared - sum_of_expansion * expansion_mean) / (n - 1);

      // Dump methods whose size is a certain number of standard deviations from the mean
      size_t dumped_values = 0;
      size_t skipped_values = 0;
      for (size_t i = 100; i > 0; i--) {  // i is the current number of standard deviations
        size_t cur_size_variance = i * i * size_variance;
        bool first = true;
        for (size_t j = 0; j < n; j++) {
          size_t cur_size = method_outlier_size[j];
          if (cur_size > size_mean) {
            size_t cur_var = cur_size - size_mean;
            cur_var = cur_var * cur_var;
            if (cur_var > cur_size_variance) {
              if (dumped_values > 20) {
                if (i == 1) {
                  skipped_values++;
                } else {
                  i = 2;  // jump to counting for 1 standard deviation
                  break;
                }
              } else {
                if (first) {
                  os << "\nBig methods (size > " << i << " standard deviations the norm):"
                     << std::endl;
                  first = false;
                }
                os << "\t" << PrettyMethod(method_outlier[j]) << " requires storage of "
                    << PrettySize(cur_size) << std::endl;
                method_outlier_size[j] = 0;  // don't consider this method again
                dumped_values++;
              }
            }
          }
        }
      }
      if (skipped_values > 0) {
        os << "\t... skipped " << skipped_values
           << " methods with size > 1 standard deviation from the norm" << std::endl;
      }
      os << std::endl << std::flush;

      // Dump methods whose expansion is a certain number of standard deviations from the mean
      dumped_values = 0;
      skipped_values = 0;
      for (size_t i = 10; i > 0; i--) {  // i is the current number of standard deviations
        double cur_expansion_variance = i * i * expansion_variance;
        bool first = true;
        for (size_t j = 0; j < n; j++) {
          double cur_expansion = method_outlier_expansion[j];
          if (cur_expansion > expansion_mean) {
            size_t cur_var = cur_expansion - expansion_mean;
            cur_var = cur_var * cur_var;
            if (cur_var > cur_expansion_variance) {
              if (dumped_values > 20) {
                if (i == 1) {
                  skipped_values++;
                } else {
                  i = 2;  // jump to counting for 1 standard deviation
                  break;
                }
              } else {
                if (first) {
                  os << "\nLarge expansion methods (size > " << i
                      << " standard deviations the norm):" << std::endl;
                  first = false;
                }
                os << "\t" << PrettyMethod(method_outlier[j]) << " expanded code by "
                    << cur_expansion << std::endl;
                method_outlier_expansion[j] = 0.0;  // don't consider this method again
                dumped_values++;
              }
            }
          }
        }
      }
      if (skipped_values > 0) {
        os << "\t... skipped " << skipped_values
           << " methods with expansion > 1 standard deviation from the norm" << std::endl;
      }
      os << std::endl << std::flush;
    }

    void Dump(std::ostream& os) {
      os << "\tart_file_bytes = " << PrettySize(file_bytes) << std::endl << std::endl
         << "\tart_file_bytes = header_bytes + object_bytes + alignment_bytes" << std::endl
         << StringPrintf("\theader_bytes    =  %8zd (%2.0f%% of art file bytes)\n"
                         "\tobject_bytes    =  %8zd (%2.0f%% of art file bytes)\n"
                         "\talignment_bytes =  %8zd (%2.0f%% of art file bytes)\n",
                         header_bytes, PercentOfFileBytes(header_bytes),
                         object_bytes, PercentOfFileBytes(object_bytes),
                         alignment_bytes, PercentOfFileBytes(alignment_bytes))
         << std::endl << std::flush;

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
      os << std::endl << std::flush;
      CHECK_EQ(object_bytes, object_bytes_total);

      os << StringPrintf("\tmanaged_code_bytes           = %8zd (%2.0f%% of oat file bytes)\n"
                         "\tmanaged_to_native_code_bytes = %8zd (%2.0f%% of oat file bytes)\n"
                         "\tnative_to_managed_code_bytes = %8zd (%2.0f%% of oat file bytes)\n",
                         managed_code_bytes, PercentOfOatBytes(managed_code_bytes),
                         managed_to_native_code_bytes, PercentOfOatBytes(managed_to_native_code_bytes),
                         native_to_managed_code_bytes, PercentOfOatBytes(native_to_managed_code_bytes))
         << std::endl << std::flush;

      os << StringPrintf("\tgc_map_bytes           = %7zd (%2.0f%% of oat file_bytes)\n"
                         "\tpc_mapping_table_bytes = %7zd (%2.0f%% of oat file_bytes)\n"
                         "\tvmap_table_bytes       = %7zd (%2.0f%% of oat file_bytes)\n",
                         gc_map_bytes, PercentOfOatBytes(gc_map_bytes),
                         pc_mapping_table_bytes, PercentOfOatBytes(pc_mapping_table_bytes),
                         vmap_table_bytes, PercentOfOatBytes(vmap_table_bytes))
         << std::endl << std::flush;

      os << StringPrintf("\tdex_instruction_bytes = %zd\n", dex_instruction_bytes);
      os << StringPrintf("\tmanaged_code_bytes expansion = %.2f (ignoring deduplication %.2f)\n",
          static_cast<double>(managed_code_bytes) / static_cast<double>(dex_instruction_bytes),
          static_cast<double>(managed_code_bytes_ignoring_deduplication) /
            static_cast<double>(dex_instruction_bytes));
      os << std::endl << std::flush;

      DumpOutliers(os);
    }
  } stats_;

 private:
  UniquePtr<OatDumper> oat_dumper_;
  std::ostream& os_;
  const std::string image_filename_;
  const std::string host_prefix_;
  Space& image_space_;
  const ImageHeader& image_header_;

  DISALLOW_COPY_AND_ASSIGN(ImageDumper);
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
  std::string elf_filename_prefix;
  UniquePtr<std::string> host_prefix;
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
    } else if (option.starts_with("--extract-elf-to=")) {
      elf_filename_prefix = option.substr(strlen("--extract-elf-to=")).data();
    } else if (option.starts_with("--host-prefix=")) {
      host_prefix.reset(new std::string(option.substr(strlen("--host-prefix=")).data()));
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

  if (host_prefix.get() == NULL) {
    const char* android_product_out = getenv("ANDROID_PRODUCT_OUT");
    if (android_product_out != NULL) {
        host_prefix.reset(new std::string(android_product_out));
    } else {
        host_prefix.reset(new std::string(""));
    }
  }

  if (oat_filename != NULL) {
    OatFile* oat_file = OatFile::Open(oat_filename, oat_filename, NULL);
    if (oat_file == NULL) {
      fprintf(stderr, "Failed to open oat file from %s\n", oat_filename);
      return EXIT_FAILURE;
    }
    OatDumper oat_dumper(*host_prefix.get(), *oat_file);
    oat_dumper.Dump(*os);

    if (!elf_filename_prefix.empty()) {
      uint32_t elf_image_count = oat_file->GetOatHeader().GetElfImageCount();
      for (uint32_t i = 0; i < elf_image_count; ++i) {
        const OatFile::OatElfImage* elf_image = oat_file->GetOatElfImage(i);

        std::string elf_filename(
            StringPrintf("%s-%u", elf_filename_prefix.c_str(), i));

        UniquePtr<File> elf_file(OS::OpenFile(elf_filename.c_str(), true));

        if (!elf_file->WriteFully(elf_image->begin(), elf_image->size())) {
          fprintf(stderr, "Failed to write ELF image to: %s\n",
                  elf_filename.c_str());
        }
      }
    }
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

  if (!host_prefix->empty()) {
    options.push_back(std::make_pair("host-prefix", host_prefix->c_str()));
  }

  UniquePtr<Runtime> runtime(Runtime::Create(options, false));
  if (runtime.get() == NULL) {
    fprintf(stderr, "Failed to create runtime\n");
    return EXIT_FAILURE;
  }

  Heap* heap = Runtime::Current()->GetHeap();
  ImageSpace* image_space = heap->GetImageSpace();
  CHECK(image_space != NULL);
  const ImageHeader& image_header = image_space->GetImageHeader();
  if (!image_header.IsValid()) {
    fprintf(stderr, "Invalid image header %s\n", image_filename);
    return EXIT_FAILURE;
  }
  ImageDumper image_dumper(*os, image_filename, *host_prefix.get(), *image_space, image_header);
  image_dumper.Dump();
  return EXIT_SUCCESS;
}

} // namespace art

int main(int argc, char** argv) {
  return art::oatdump(argc, argv);
}
