/*
 * Copyright (C) 2014 The Android Open Source Project
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
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <unordered_set>

#include "art_method-inl.h"
#include "base/unix_file/fd_file.h"
#include "base/stringprintf.h"
#include "gc/space/image_space.h"
#include "gc/heap.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "image.h"
#include "scoped_thread_state_change.h"
#include "os.h"

#include "cmdline.h"
#include "backtrace/BacktraceMap.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>

namespace art {

class ImgDiagDumper {
 public:
  explicit ImgDiagDumper(std::ostream* os,
                         const ImageHeader& image_header,
                         const std::string& image_location,
                         pid_t image_diff_pid,
                         pid_t zygote_diff_pid)
      : os_(os),
        image_header_(image_header),
        image_location_(image_location),
        image_diff_pid_(image_diff_pid),
        zygote_diff_pid_(zygote_diff_pid) {}

  bool Dump() SHARED_REQUIRES(Locks::mutator_lock_) {
    std::ostream& os = *os_;
    os << "IMAGE LOCATION: " << image_location_ << "\n\n";

    os << "MAGIC: " << image_header_.GetMagic() << "\n\n";

    os << "IMAGE BEGIN: " << reinterpret_cast<void*>(image_header_.GetImageBegin()) << "\n\n";

    bool ret = true;
    if (image_diff_pid_ >= 0) {
      os << "IMAGE DIFF PID (" << image_diff_pid_ << "): ";
      ret = DumpImageDiff(image_diff_pid_, zygote_diff_pid_);
      os << "\n\n";
    } else {
      os << "IMAGE DIFF PID: disabled\n\n";
    }

    os << std::flush;

    return ret;
  }

 private:
  static bool EndsWith(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  // Return suffix of the file path after the last /. (e.g. /foo/bar -> bar, bar -> bar)
  static std::string BaseName(const std::string& str) {
    size_t idx = str.rfind("/");
    if (idx == std::string::npos) {
      return str;
    }

    return str.substr(idx + 1);
  }

  bool DumpImageDiff(pid_t image_diff_pid, pid_t zygote_diff_pid)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    std::ostream& os = *os_;

    {
      struct stat sts;
      std::string proc_pid_str =
          StringPrintf("/proc/%ld", static_cast<long>(image_diff_pid));  // NOLINT [runtime/int]
      if (stat(proc_pid_str.c_str(), &sts) == -1) {
        os << "Process does not exist";
        return false;
      }
    }

    // Open /proc/$pid/maps to view memory maps
    auto proc_maps = std::unique_ptr<BacktraceMap>(BacktraceMap::Create(image_diff_pid));
    if (proc_maps == nullptr) {
      os << "Could not read backtrace maps";
      return false;
    }

    bool found_boot_map = false;
    backtrace_map_t boot_map = backtrace_map_t();
    // Find the memory map only for boot.art
    for (const backtrace_map_t& map : *proc_maps) {
      if (EndsWith(map.name, GetImageLocationBaseName())) {
        if ((map.flags & PROT_WRITE) != 0) {
          boot_map = map;
          found_boot_map = true;
          break;
        }
        // In actuality there's more than 1 map, but the second one is read-only.
        // The one we care about is the write-able map.
        // The readonly maps are guaranteed to be identical, so its not interesting to compare
        // them.
      }
    }

    if (!found_boot_map) {
      os << "Could not find map for " << GetImageLocationBaseName();
      return false;
    }

    // Future idea: diff against zygote so we can ignore the shared dirty pages.
    return DumpImageDiffMap(image_diff_pid, zygote_diff_pid, boot_map);
  }

  static std::string PrettyFieldValue(ArtField* field, mirror::Object* obj)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    std::ostringstream oss;
    switch (field->GetTypeAsPrimitiveType()) {
      case Primitive::kPrimNot: {
        oss << obj->GetFieldObject<mirror::Object, kVerifyNone, kWithoutReadBarrier>(
            field->GetOffset());
        break;
      }
      case Primitive::kPrimBoolean: {
        oss << static_cast<bool>(obj->GetFieldBoolean<kVerifyNone>(field->GetOffset()));
        break;
      }
      case Primitive::kPrimByte: {
        oss << static_cast<int32_t>(obj->GetFieldByte<kVerifyNone>(field->GetOffset()));
        break;
      }
      case Primitive::kPrimChar: {
        oss << obj->GetFieldChar<kVerifyNone>(field->GetOffset());
        break;
      }
      case Primitive::kPrimShort: {
        oss << obj->GetFieldShort<kVerifyNone>(field->GetOffset());
        break;
      }
      case Primitive::kPrimInt: {
        oss << obj->GetField32<kVerifyNone>(field->GetOffset());
        break;
      }
      case Primitive::kPrimLong: {
        oss << obj->GetField64<kVerifyNone>(field->GetOffset());
        break;
      }
      case Primitive::kPrimFloat: {
        oss << obj->GetField32<kVerifyNone>(field->GetOffset());
        break;
      }
      case Primitive::kPrimDouble: {
        oss << obj->GetField64<kVerifyNone>(field->GetOffset());
        break;
      }
      case Primitive::kPrimVoid: {
        oss << "void";
        break;
      }
    }
    return oss.str();
  }

  // Aggregate and detail class data from an image diff.
  struct ClassData {
    int dirty_object_count = 0;

    // Track only the byte-per-byte dirtiness (in bytes)
    int dirty_object_byte_count = 0;

    // Track the object-by-object dirtiness (in bytes)
    int dirty_object_size_in_bytes = 0;

    int clean_object_count = 0;

    std::string descriptor;

    int false_dirty_byte_count = 0;
    int false_dirty_object_count = 0;
    std::vector<mirror::Object*> false_dirty_objects;

    // Remote pointers to dirty objects
    std::vector<mirror::Object*> dirty_objects;
  };

  void DiffObjectContents(mirror::Object* obj,
                          uint8_t* remote_bytes,
                          std::ostream& os) SHARED_REQUIRES(Locks::mutator_lock_) {
    const char* tabs = "    ";
    // Attempt to find fields for all dirty bytes.
    mirror::Class* klass = obj->GetClass();
    if (obj->IsClass()) {
      os << tabs << "Class " << PrettyClass(obj->AsClass()) << " " << obj << "\n";
    } else {
      os << tabs << "Instance of " << PrettyClass(klass) << " " << obj << "\n";
    }

    std::unordered_set<ArtField*> dirty_instance_fields;
    std::unordered_set<ArtField*> dirty_static_fields;
    const uint8_t* obj_bytes = reinterpret_cast<const uint8_t*>(obj);
    mirror::Object* remote_obj = reinterpret_cast<mirror::Object*>(remote_bytes);
    for (size_t i = 0, count = obj->SizeOf(); i < count; ++i) {
      if (obj_bytes[i] != remote_bytes[i]) {
        ArtField* field = ArtField::FindInstanceFieldWithOffset</*exact*/false>(klass, i);
        if (field != nullptr) {
          dirty_instance_fields.insert(field);
        } else if (obj->IsClass()) {
          field = ArtField::FindStaticFieldWithOffset</*exact*/false>(obj->AsClass(), i);
          if (field != nullptr) {
            dirty_static_fields.insert(field);
          }
        }
        if (field == nullptr) {
          if (klass->IsArrayClass()) {
            mirror::Class* component_type = klass->GetComponentType();
            Primitive::Type primitive_type = component_type->GetPrimitiveType();
            size_t component_size = Primitive::ComponentSize(primitive_type);
            size_t data_offset = mirror::Array::DataOffset(component_size).Uint32Value();
            if (i >= data_offset) {
              os << tabs << "Dirty array element " << (i - data_offset) / component_size << "\n";
              // Skip to next element to prevent spam.
              i += component_size - 1;
              continue;
            }
          }
          os << tabs << "No field for byte offset " << i << "\n";
        }
      }
    }
    // Dump different fields. TODO: Dump field contents.
    if (!dirty_instance_fields.empty()) {
      os << tabs << "Dirty instance fields " << dirty_instance_fields.size() << "\n";
      for (ArtField* field : dirty_instance_fields) {
        os << tabs << PrettyField(field)
           << " original=" << PrettyFieldValue(field, obj)
           << " remote=" << PrettyFieldValue(field, remote_obj) << "\n";
      }
    }
    if (!dirty_static_fields.empty()) {
      os << tabs << "Dirty static fields " << dirty_static_fields.size() << "\n";
      for (ArtField* field : dirty_static_fields) {
        os << tabs << PrettyField(field)
           << " original=" << PrettyFieldValue(field, obj)
           << " remote=" << PrettyFieldValue(field, remote_obj) << "\n";
      }
    }
    os << "\n";
  }

  // Look at /proc/$pid/mem and only diff the things from there
  bool DumpImageDiffMap(pid_t image_diff_pid,
                        pid_t zygote_diff_pid,
                        const backtrace_map_t& boot_map)
    SHARED_REQUIRES(Locks::mutator_lock_) {
    std::ostream& os = *os_;
    const size_t pointer_size = InstructionSetPointerSize(
        Runtime::Current()->GetInstructionSet());

    std::string file_name =
        StringPrintf("/proc/%ld/mem", static_cast<long>(image_diff_pid));  // NOLINT [runtime/int]

    size_t boot_map_size = boot_map.end - boot_map.start;

    // Open /proc/$pid/mem as a file
    auto map_file = std::unique_ptr<File>(OS::OpenFileForReading(file_name.c_str()));
    if (map_file == nullptr) {
      os << "Failed to open " << file_name << " for reading";
      return false;
    }

    // Memory-map /proc/$pid/mem subset from the boot map
    CHECK(boot_map.end >= boot_map.start);

    std::string error_msg;

    // Walk the bytes and diff against our boot image
    const ImageHeader& boot_image_header = image_header_;

    os << "\nObserving boot image header at address "
       << reinterpret_cast<const void*>(&boot_image_header)
       << "\n\n";

    const uint8_t* image_begin_unaligned = boot_image_header.GetImageBegin();
    const uint8_t* image_mirror_end_unaligned = image_begin_unaligned +
        boot_image_header.GetImageSection(ImageHeader::kSectionObjects).Size();
    const uint8_t* image_end_unaligned = image_begin_unaligned + boot_image_header.GetImageSize();

    // Adjust range to nearest page
    const uint8_t* image_begin = AlignDown(image_begin_unaligned, kPageSize);
    const uint8_t* image_end = AlignUp(image_end_unaligned, kPageSize);

    ptrdiff_t page_off_begin = boot_image_header.GetImageBegin() - image_begin;

    if (reinterpret_cast<uintptr_t>(image_begin) > boot_map.start ||
        reinterpret_cast<uintptr_t>(image_end) < boot_map.end) {
      // Sanity check that we aren't trying to read a completely different boot image
      os << "Remote boot map is out of range of local boot map: " <<
        "local begin " << reinterpret_cast<const void*>(image_begin) <<
        ", local end " << reinterpret_cast<const void*>(image_end) <<
        ", remote begin " << reinterpret_cast<const void*>(boot_map.start) <<
        ", remote end " << reinterpret_cast<const void*>(boot_map.end);
      return false;
      // If we wanted even more validation we could map the ImageHeader from the file
    }

    std::vector<uint8_t> remote_contents(boot_map_size);
    if (!map_file->PreadFully(&remote_contents[0], boot_map_size, boot_map.start)) {
      os << "Could not fully read file " << file_name;
      return false;
    }

    std::vector<uint8_t> zygote_contents;
    std::unique_ptr<File> zygote_map_file;
    if (zygote_diff_pid != -1) {
      std::string zygote_file_name =
          StringPrintf("/proc/%ld/mem", static_cast<long>(zygote_diff_pid));  // NOLINT [runtime/int]
      zygote_map_file.reset(OS::OpenFileForReading(zygote_file_name.c_str()));
      // The boot map should be at the same address.
      zygote_contents.resize(boot_map_size);
      if (!zygote_map_file->PreadFully(&zygote_contents[0], boot_map_size, boot_map.start)) {
        LOG(WARNING) << "Could not fully read zygote file " << zygote_file_name;
        zygote_contents.clear();
      }
    }

    std::string page_map_file_name = StringPrintf(
        "/proc/%ld/pagemap", static_cast<long>(image_diff_pid));  // NOLINT [runtime/int]
    auto page_map_file = std::unique_ptr<File>(OS::OpenFileForReading(page_map_file_name.c_str()));
    if (page_map_file == nullptr) {
      os << "Failed to open " << page_map_file_name << " for reading: " << strerror(errno);
      return false;
    }

    // Not truly clean, mmap-ing boot.art again would be more pristine, but close enough
    const char* clean_page_map_file_name = "/proc/self/pagemap";
    auto clean_page_map_file = std::unique_ptr<File>(
        OS::OpenFileForReading(clean_page_map_file_name));
    if (clean_page_map_file == nullptr) {
      os << "Failed to open " << clean_page_map_file_name << " for reading: " << strerror(errno);
      return false;
    }

    auto kpage_flags_file = std::unique_ptr<File>(OS::OpenFileForReading("/proc/kpageflags"));
    if (kpage_flags_file == nullptr) {
      os << "Failed to open /proc/kpageflags for reading: " << strerror(errno);
      return false;
    }

    auto kpage_count_file = std::unique_ptr<File>(OS::OpenFileForReading("/proc/kpagecount"));
    if (kpage_count_file == nullptr) {
      os << "Failed to open /proc/kpagecount for reading:" << strerror(errno);
      return false;
    }

    // Set of the remote virtual page indices that are dirty
    std::set<size_t> dirty_page_set_remote;
    // Set of the local virtual page indices that are dirty
    std::set<size_t> dirty_page_set_local;

    size_t different_int32s = 0;
    size_t different_bytes = 0;
    size_t different_pages = 0;
    size_t virtual_page_idx = 0;   // Virtual page number (for an absolute memory address)
    size_t page_idx = 0;           // Page index relative to 0
    size_t previous_page_idx = 0;  // Previous page index relative to 0
    size_t dirty_pages = 0;
    size_t private_pages = 0;
    size_t private_dirty_pages = 0;

    // Iterate through one page at a time. Boot map begin/end already implicitly aligned.
    for (uintptr_t begin = boot_map.start; begin != boot_map.end; begin += kPageSize) {
      ptrdiff_t offset = begin - boot_map.start;

      // We treat the image header as part of the memory map for now
      // If we wanted to change this, we could pass base=start+sizeof(ImageHeader)
      // But it might still be interesting to see if any of the ImageHeader data mutated
      const uint8_t* local_ptr = reinterpret_cast<const uint8_t*>(&boot_image_header) + offset;
      uint8_t* remote_ptr = &remote_contents[offset];

      if (memcmp(local_ptr, remote_ptr, kPageSize) != 0) {
        different_pages++;

        // Count the number of 32-bit integers that are different.
        for (size_t i = 0; i < kPageSize / sizeof(uint32_t); ++i) {
          uint32_t* remote_ptr_int32 = reinterpret_cast<uint32_t*>(remote_ptr);
          const uint32_t* local_ptr_int32 = reinterpret_cast<const uint32_t*>(local_ptr);

          if (remote_ptr_int32[i] != local_ptr_int32[i]) {
            different_int32s++;
          }
        }
      }
    }

    // Iterate through one byte at a time.
    for (uintptr_t begin = boot_map.start; begin != boot_map.end; ++begin) {
      previous_page_idx = page_idx;
      ptrdiff_t offset = begin - boot_map.start;

      // We treat the image header as part of the memory map for now
      // If we wanted to change this, we could pass base=start+sizeof(ImageHeader)
      // But it might still be interesting to see if any of the ImageHeader data mutated
      const uint8_t* local_ptr = reinterpret_cast<const uint8_t*>(&boot_image_header) + offset;
      uint8_t* remote_ptr = &remote_contents[offset];

      virtual_page_idx = reinterpret_cast<uintptr_t>(local_ptr) / kPageSize;

      // Calculate the page index, relative to the 0th page where the image begins
      page_idx = (offset + page_off_begin) / kPageSize;
      if (*local_ptr != *remote_ptr) {
        // Track number of bytes that are different
        different_bytes++;
      }

      // Independently count the # of dirty pages on the remote side
      size_t remote_virtual_page_idx = begin / kPageSize;
      if (previous_page_idx != page_idx) {
        uint64_t page_count = 0xC0FFEE;
        // TODO: virtual_page_idx needs to be from the same process
        int dirtiness = (IsPageDirty(page_map_file.get(),        // Image-diff-pid procmap
                                     clean_page_map_file.get(),  // Self procmap
                                     kpage_flags_file.get(),
                                     kpage_count_file.get(),
                                     remote_virtual_page_idx,    // potentially "dirty" page
                                     virtual_page_idx,           // true "clean" page
                                     &page_count,
                                     &error_msg));
        if (dirtiness < 0) {
          os << error_msg;
          return false;
        } else if (dirtiness > 0) {
          dirty_pages++;
          dirty_page_set_remote.insert(dirty_page_set_remote.end(), remote_virtual_page_idx);
          dirty_page_set_local.insert(dirty_page_set_local.end(), virtual_page_idx);
        }

        bool is_dirty = dirtiness > 0;
        bool is_private = page_count == 1;

        if (page_count == 1) {
          private_pages++;
        }

        if (is_dirty && is_private) {
          private_dirty_pages++;
        }
      }
    }

    std::map<mirror::Class*, ClassData> class_data;

    // Walk each object in the remote image space and compare it against ours
    size_t different_objects = 0;

    std::map<off_t /* field offset */, int /* count */> art_method_field_dirty_count;
    std::vector<ArtMethod*> art_method_dirty_objects;

    std::map<off_t /* field offset */, int /* count */> class_field_dirty_count;
    std::vector<mirror::Class*> class_dirty_objects;

    // List of local objects that are clean, but located on dirty pages.
    std::vector<mirror::Object*> false_dirty_objects;
    size_t false_dirty_object_bytes = 0;

    // Look up remote classes by their descriptor
    std::map<std::string, mirror::Class*> remote_class_map;
    // Look up local classes by their descriptor
    std::map<std::string, mirror::Class*> local_class_map;

    // Objects that are dirty against the image (possibly shared or private dirty).
    std::set<mirror::Object*> image_dirty_objects;

    // Objects that are dirty against the zygote (probably private dirty).
    std::set<mirror::Object*> zygote_dirty_objects;

    size_t dirty_object_bytes = 0;
    const uint8_t* begin_image_ptr = image_begin_unaligned;
    const uint8_t* end_image_ptr = image_mirror_end_unaligned;

    const uint8_t* current = begin_image_ptr + RoundUp(sizeof(ImageHeader), kObjectAlignment);
    while (reinterpret_cast<uintptr_t>(current) < reinterpret_cast<uintptr_t>(end_image_ptr)) {
      CHECK_ALIGNED(current, kObjectAlignment);
      mirror::Object* obj = reinterpret_cast<mirror::Object*>(const_cast<uint8_t*>(current));

      // Sanity check that we are reading a real object
      CHECK(obj->GetClass() != nullptr) << "Image object at address " << obj << " has null class";
      if (kUseBakerOrBrooksReadBarrier) {
        obj->AssertReadBarrierPointer();
      }

      // Iterate every page this object belongs to
      bool on_dirty_page = false;
      size_t page_off = 0;
      size_t current_page_idx;
      uintptr_t object_address;
      do {
        object_address = reinterpret_cast<uintptr_t>(current);
        current_page_idx = object_address / kPageSize + page_off;

        if (dirty_page_set_local.find(current_page_idx) != dirty_page_set_local.end()) {
          // This object is on a dirty page
          on_dirty_page = true;
        }

        page_off++;
      } while ((current_page_idx * kPageSize) <
               RoundUp(object_address + obj->SizeOf(), kObjectAlignment));

      mirror::Class* klass = obj->GetClass();

      // Check against the other object and see if they are different
      ptrdiff_t offset = current - begin_image_ptr;
      const uint8_t* current_remote = &remote_contents[offset];
      mirror::Object* remote_obj = reinterpret_cast<mirror::Object*>(
          const_cast<uint8_t*>(current_remote));

      bool different_image_object = memcmp(current, current_remote, obj->SizeOf()) != 0;
      if (different_image_object) {
        bool different_zygote_object = false;
        if (!zygote_contents.empty()) {
          const uint8_t* zygote_ptr = &zygote_contents[offset];
          different_zygote_object = memcmp(current, zygote_ptr, obj->SizeOf()) != 0;
        }
        if (different_zygote_object) {
          // Different from zygote.
          zygote_dirty_objects.insert(obj);
        } else {
          // Just different from iamge.
          image_dirty_objects.insert(obj);
        }

        different_objects++;
        dirty_object_bytes += obj->SizeOf();

        ++class_data[klass].dirty_object_count;

        // Go byte-by-byte and figure out what exactly got dirtied
        size_t dirty_byte_count_per_object = 0;
        for (size_t i = 0; i < obj->SizeOf(); ++i) {
          if (current[i] != current_remote[i]) {
            dirty_byte_count_per_object++;
          }
        }
        class_data[klass].dirty_object_byte_count += dirty_byte_count_per_object;
        class_data[klass].dirty_object_size_in_bytes += obj->SizeOf();
        class_data[klass].dirty_objects.push_back(remote_obj);
      } else {
        ++class_data[klass].clean_object_count;
      }

      std::string descriptor = GetClassDescriptor(klass);
      if (different_image_object) {
        if (klass->IsClassClass()) {
          // this is a "Class"
          mirror::Class* obj_as_class  = reinterpret_cast<mirror::Class*>(remote_obj);

          // print the fields that are dirty
          for (size_t i = 0; i < obj->SizeOf(); ++i) {
            if (current[i] != current_remote[i]) {
              class_field_dirty_count[i]++;
            }
          }

          class_dirty_objects.push_back(obj_as_class);
        } else if (strcmp(descriptor.c_str(), "Ljava/lang/reflect/ArtMethod;") == 0) {
          // this is an ArtMethod
          ArtMethod* art_method = reinterpret_cast<ArtMethod*>(remote_obj);

          // print the fields that are dirty
          for (size_t i = 0; i < obj->SizeOf(); ++i) {
            if (current[i] != current_remote[i]) {
              art_method_field_dirty_count[i]++;
            }
          }

          art_method_dirty_objects.push_back(art_method);
        }
      } else if (on_dirty_page) {
        // This object was either never mutated or got mutated back to the same value.
        // TODO: Do I want to distinguish a "different" vs a "dirty" page here?
        false_dirty_objects.push_back(obj);
        class_data[klass].false_dirty_objects.push_back(obj);
        false_dirty_object_bytes += obj->SizeOf();
        class_data[obj->GetClass()].false_dirty_byte_count += obj->SizeOf();
        class_data[obj->GetClass()].false_dirty_object_count += 1;
      }

      if (strcmp(descriptor.c_str(), "Ljava/lang/Class;") == 0) {
        local_class_map[descriptor] = reinterpret_cast<mirror::Class*>(obj);
        remote_class_map[descriptor] = reinterpret_cast<mirror::Class*>(remote_obj);
      }

      // Unconditionally store the class descriptor in case we need it later
      class_data[klass].descriptor = descriptor;
      current += RoundUp(obj->SizeOf(), kObjectAlignment);
    }

    // Looking at only dirty pages, figure out how many of those bytes belong to dirty objects.
    float true_dirtied_percent = dirty_object_bytes * 1.0f / (dirty_pages * kPageSize);
    size_t false_dirty_pages = dirty_pages - different_pages;

    os << "Mapping at [" << reinterpret_cast<void*>(boot_map.start) << ", "
       << reinterpret_cast<void*>(boot_map.end) << ") had: \n  "
       << different_bytes << " differing bytes, \n  "
       << different_int32s << " differing int32s, \n  "
       << different_objects << " different objects, \n  "
       << dirty_object_bytes << " different object [bytes], \n  "
       << false_dirty_objects.size() << " false dirty objects,\n  "
       << false_dirty_object_bytes << " false dirty object [bytes], \n  "
       << true_dirtied_percent << " different objects-vs-total in a dirty page;\n  "
       << different_pages << " different pages; \n  "
       << dirty_pages << " pages are dirty; \n  "
       << false_dirty_pages << " pages are false dirty; \n  "
       << private_pages << " pages are private; \n  "
       << private_dirty_pages << " pages are Private_Dirty\n  "
       << "";

    // vector of pairs (int count, Class*)
    auto dirty_object_class_values = SortByValueDesc<mirror::Class*, int, ClassData>(
        class_data, [](const ClassData& d) { return d.dirty_object_count; });
    auto clean_object_class_values = SortByValueDesc<mirror::Class*, int, ClassData>(
        class_data, [](const ClassData& d) { return d.clean_object_count; });

    if (!zygote_dirty_objects.empty()) {
      os << "\n" << "  Dirty objects compared to zygote (probably private dirty): "
         << zygote_dirty_objects.size() << "\n";
      for (mirror::Object* obj : zygote_dirty_objects) {
        const uint8_t* obj_bytes = reinterpret_cast<const uint8_t*>(obj);
        ptrdiff_t offset = obj_bytes - begin_image_ptr;
        uint8_t* remote_bytes = &zygote_contents[offset];
        DiffObjectContents(obj, remote_bytes, os);
      }
    }
    os << "\n" << "  Dirty objects compared to image (private or shared dirty): "
       << image_dirty_objects.size() << "\n";
    for (mirror::Object* obj : image_dirty_objects) {
      const uint8_t* obj_bytes = reinterpret_cast<const uint8_t*>(obj);
      ptrdiff_t offset = obj_bytes - begin_image_ptr;
      uint8_t* remote_bytes = &remote_contents[offset];
      DiffObjectContents(obj, remote_bytes, os);
    }

    os << "\n" << "  Dirty object count by class:\n";
    for (const auto& vk_pair : dirty_object_class_values) {
      int dirty_object_count = vk_pair.first;
      mirror::Class* klass = vk_pair.second;
      int object_sizes = class_data[klass].dirty_object_size_in_bytes;
      float avg_dirty_bytes_per_class =
          class_data[klass].dirty_object_byte_count * 1.0f / object_sizes;
      float avg_object_size = object_sizes * 1.0f / dirty_object_count;
      const std::string& descriptor = class_data[klass].descriptor;
      os << "    " << PrettyClass(klass) << " ("
         << "objects: " << dirty_object_count << ", "
         << "avg dirty bytes: " << avg_dirty_bytes_per_class << ", "
         << "avg object size: " << avg_object_size << ", "
         << "class descriptor: '" << descriptor << "'"
         << ")\n";

      constexpr size_t kMaxAddressPrint = 5;
      if (strcmp(descriptor.c_str(), "Ljava/lang/reflect/ArtMethod;") == 0) {
        os << "      sample object addresses: ";
        for (size_t i = 0; i < art_method_dirty_objects.size() && i < kMaxAddressPrint; ++i) {
          auto art_method = art_method_dirty_objects[i];

          os << reinterpret_cast<void*>(art_method) << ", ";
        }
        os << "\n";

        os << "      dirty byte +offset:count list = ";
        auto art_method_field_dirty_count_sorted =
            SortByValueDesc<off_t, int, int>(art_method_field_dirty_count);
        for (auto pair : art_method_field_dirty_count_sorted) {
          off_t offset = pair.second;
          int count = pair.first;

          os << "+" << offset << ":" << count << ", ";
        }

        os << "\n";

        os << "      field contents:\n";
        const auto& dirty_objects_list = class_data[klass].dirty_objects;
        for (mirror::Object* obj : dirty_objects_list) {
          // remote method
          auto art_method = reinterpret_cast<ArtMethod*>(obj);

          // remote class
          mirror::Class* remote_declaring_class =
            FixUpRemotePointer(art_method->GetDeclaringClass(), remote_contents, boot_map);

          // local class
          mirror::Class* declaring_class =
            RemoteContentsPointerToLocal(remote_declaring_class,
                                         remote_contents,
                                         boot_image_header);

          os << "        " << reinterpret_cast<void*>(obj) << " ";
          os << "  entryPointFromJni: "
             << reinterpret_cast<const void*>(
                    art_method->GetEntryPointFromJniPtrSize(pointer_size)) << ", ";
          os << "  entryPointFromQuickCompiledCode: "
             << reinterpret_cast<const void*>(
                    art_method->GetEntryPointFromQuickCompiledCodePtrSize(pointer_size))
             << ", ";
          os << "  isNative? " << (art_method->IsNative() ? "yes" : "no") << ", ";
          os << "  class_status (local): " << declaring_class->GetStatus();
          os << "  class_status (remote): " << remote_declaring_class->GetStatus();
          os << "\n";
        }
      }
      if (strcmp(descriptor.c_str(), "Ljava/lang/Class;") == 0) {
        os << "       sample object addresses: ";
        for (size_t i = 0; i < class_dirty_objects.size() && i < kMaxAddressPrint; ++i) {
          auto class_ptr = class_dirty_objects[i];

          os << reinterpret_cast<void*>(class_ptr) << ", ";
        }
        os << "\n";

        os << "       dirty byte +offset:count list = ";
        auto class_field_dirty_count_sorted =
            SortByValueDesc<off_t, int, int>(class_field_dirty_count);
        for (auto pair : class_field_dirty_count_sorted) {
          off_t offset = pair.second;
          int count = pair.first;

          os << "+" << offset << ":" << count << ", ";
        }
        os << "\n";

        os << "      field contents:\n";
        const auto& dirty_objects_list = class_data[klass].dirty_objects;
        for (mirror::Object* obj : dirty_objects_list) {
          // remote class object
          auto remote_klass = reinterpret_cast<mirror::Class*>(obj);

          // local class object
          auto local_klass = RemoteContentsPointerToLocal(remote_klass,
                                                          remote_contents,
                                                          boot_image_header);

          os << "        " << reinterpret_cast<void*>(obj) << " ";
          os << "  class_status (remote): " << remote_klass->GetStatus() << ", ";
          os << "  class_status (local): " << local_klass->GetStatus();
          os << "\n";
        }
      }
    }

    auto false_dirty_object_class_values = SortByValueDesc<mirror::Class*, int, ClassData>(
        class_data, [](const ClassData& d) { return d.false_dirty_object_count; });

    os << "\n" << "  False-dirty object count by class:\n";
    for (const auto& vk_pair : false_dirty_object_class_values) {
      int object_count = vk_pair.first;
      mirror::Class* klass = vk_pair.second;
      int object_sizes = class_data[klass].false_dirty_byte_count;
      float avg_object_size = object_sizes * 1.0f / object_count;
      const std::string& descriptor = class_data[klass].descriptor;
      os << "    " << PrettyClass(klass) << " ("
         << "objects: " << object_count << ", "
         << "avg object size: " << avg_object_size << ", "
         << "total bytes: " << object_sizes << ", "
         << "class descriptor: '" << descriptor << "'"
         << ")\n";

      if (strcmp(descriptor.c_str(), "Ljava/lang/reflect/ArtMethod;") == 0) {
        auto& art_method_false_dirty_objects = class_data[klass].false_dirty_objects;

        os << "      field contents:\n";
        for (mirror::Object* obj : art_method_false_dirty_objects) {
          // local method
          auto art_method = reinterpret_cast<ArtMethod*>(obj);

          // local class
          mirror::Class* declaring_class = art_method->GetDeclaringClass();

          os << "        " << reinterpret_cast<void*>(obj) << " ";
          os << "  entryPointFromJni: "
             << reinterpret_cast<const void*>(
                    art_method->GetEntryPointFromJniPtrSize(pointer_size)) << ", ";
          os << "  entryPointFromQuickCompiledCode: "
             << reinterpret_cast<const void*>(
                    art_method->GetEntryPointFromQuickCompiledCodePtrSize(pointer_size))
             << ", ";
          os << "  isNative? " << (art_method->IsNative() ? "yes" : "no") << ", ";
          os << "  class_status (local): " << declaring_class->GetStatus();
          os << "\n";
        }
      }
    }

    os << "\n" << "  Clean object count by class:\n";
    for (const auto& vk_pair : clean_object_class_values) {
      os << "    " << PrettyClass(vk_pair.second) << " (" << vk_pair.first << ")\n";
    }

    return true;
  }

  // Fixup a remote pointer that we read from a foreign boot.art to point to our own memory.
  // Returned pointer will point to inside of remote_contents.
  template <typename T>
  static T* FixUpRemotePointer(T* remote_ptr,
                               std::vector<uint8_t>& remote_contents,
                               const backtrace_map_t& boot_map) {
    if (remote_ptr == nullptr) {
      return nullptr;
    }

    uintptr_t remote = reinterpret_cast<uintptr_t>(remote_ptr);

    CHECK_LE(boot_map.start, remote);
    CHECK_GT(boot_map.end, remote);

    off_t boot_offset = remote - boot_map.start;

    return reinterpret_cast<T*>(&remote_contents[boot_offset]);
  }

  template <typename T>
  static T* RemoteContentsPointerToLocal(T* remote_ptr,
                                         std::vector<uint8_t>& remote_contents,
                                         const ImageHeader& image_header) {
    if (remote_ptr == nullptr) {
      return nullptr;
    }

    uint8_t* remote = reinterpret_cast<uint8_t*>(remote_ptr);
    ptrdiff_t boot_offset = remote - &remote_contents[0];

    const uint8_t* local_ptr = reinterpret_cast<const uint8_t*>(&image_header) + boot_offset;

    return reinterpret_cast<T*>(const_cast<uint8_t*>(local_ptr));
  }

  static std::string GetClassDescriptor(mirror::Class* klass)
    SHARED_REQUIRES(Locks::mutator_lock_) {
    CHECK(klass != nullptr);

    std::string descriptor;
    const char* descriptor_str = klass->GetDescriptor(&descriptor);

    return std::string(descriptor_str);
  }

  template <typename K, typename V, typename D>
  static std::vector<std::pair<V, K>> SortByValueDesc(
      const std::map<K, D> map,
      std::function<V(const D&)> value_mapper = [](const D& d) { return static_cast<V>(d); }) {
    // Store value->key so that we can use the default sort from pair which
    // sorts by value first and then key
    std::vector<std::pair<V, K>> value_key_vector;

    for (const auto& kv_pair : map) {
      value_key_vector.push_back(std::make_pair(value_mapper(kv_pair.second), kv_pair.first));
    }

    // Sort in reverse (descending order)
    std::sort(value_key_vector.rbegin(), value_key_vector.rend());
    return value_key_vector;
  }

  static bool GetPageFrameNumber(File* page_map_file,
                                size_t virtual_page_index,
                                uint64_t* page_frame_number,
                                std::string* error_msg) {
    CHECK(page_map_file != nullptr);
    CHECK(page_frame_number != nullptr);
    CHECK(error_msg != nullptr);

    constexpr size_t kPageMapEntrySize = sizeof(uint64_t);
    constexpr uint64_t kPageFrameNumberMask = (1ULL << 55) - 1;  // bits 0-54 [in /proc/$pid/pagemap]
    constexpr uint64_t kPageSoftDirtyMask = (1ULL << 55);  // bit 55 [in /proc/$pid/pagemap]

    uint64_t page_map_entry = 0;

    // Read 64-bit entry from /proc/$pid/pagemap to get the physical page frame number
    if (!page_map_file->PreadFully(&page_map_entry, kPageMapEntrySize,
                                  virtual_page_index * kPageMapEntrySize)) {
      *error_msg = StringPrintf("Failed to read the virtual page index entry from %s",
                                page_map_file->GetPath().c_str());
      return false;
    }

    // TODO: seems useless, remove this.
    bool soft_dirty = (page_map_entry & kPageSoftDirtyMask) != 0;
    if ((false)) {
      LOG(VERBOSE) << soft_dirty;  // Suppress unused warning
      UNREACHABLE();
    }

    *page_frame_number = page_map_entry & kPageFrameNumberMask;

    return true;
  }

  static int IsPageDirty(File* page_map_file,
                         File* clean_page_map_file,
                         File* kpage_flags_file,
                         File* kpage_count_file,
                         size_t virtual_page_idx,
                         size_t clean_virtual_page_idx,
                         // Out parameters:
                         uint64_t* page_count, std::string* error_msg) {
    CHECK(page_map_file != nullptr);
    CHECK(clean_page_map_file != nullptr);
    CHECK_NE(page_map_file, clean_page_map_file);
    CHECK(kpage_flags_file != nullptr);
    CHECK(kpage_count_file != nullptr);
    CHECK(page_count != nullptr);
    CHECK(error_msg != nullptr);

    // Constants are from https://www.kernel.org/doc/Documentation/vm/pagemap.txt

    constexpr size_t kPageFlagsEntrySize = sizeof(uint64_t);
    constexpr size_t kPageCountEntrySize = sizeof(uint64_t);
    constexpr uint64_t kPageFlagsDirtyMask = (1ULL << 4);  // in /proc/kpageflags
    constexpr uint64_t kPageFlagsNoPageMask = (1ULL << 20);  // in /proc/kpageflags
    constexpr uint64_t kPageFlagsMmapMask = (1ULL << 11);  // in /proc/kpageflags

    uint64_t page_frame_number = 0;
    if (!GetPageFrameNumber(page_map_file, virtual_page_idx, &page_frame_number, error_msg)) {
      return -1;
    }

    uint64_t page_frame_number_clean = 0;
    if (!GetPageFrameNumber(clean_page_map_file, clean_virtual_page_idx, &page_frame_number_clean,
                            error_msg)) {
      return -1;
    }

    // Read 64-bit entry from /proc/kpageflags to get the dirty bit for a page
    uint64_t kpage_flags_entry = 0;
    if (!kpage_flags_file->PreadFully(&kpage_flags_entry,
                                     kPageFlagsEntrySize,
                                     page_frame_number * kPageFlagsEntrySize)) {
      *error_msg = StringPrintf("Failed to read the page flags from %s",
                                kpage_flags_file->GetPath().c_str());
      return -1;
    }

    // Read 64-bit entyry from /proc/kpagecount to get mapping counts for a page
    if (!kpage_count_file->PreadFully(page_count /*out*/,
                                     kPageCountEntrySize,
                                     page_frame_number * kPageCountEntrySize)) {
      *error_msg = StringPrintf("Failed to read the page count from %s",
                                kpage_count_file->GetPath().c_str());
      return -1;
    }

    // There must be a page frame at the requested address.
    CHECK_EQ(kpage_flags_entry & kPageFlagsNoPageMask, 0u);
    // The page frame must be memory mapped
    CHECK_NE(kpage_flags_entry & kPageFlagsMmapMask, 0u);

    // Page is dirty, i.e. has diverged from file, if the 4th bit is set to 1
    bool flags_dirty = (kpage_flags_entry & kPageFlagsDirtyMask) != 0;

    // page_frame_number_clean must come from the *same* process
    // but a *different* mmap than page_frame_number
    if (flags_dirty) {
      CHECK_NE(page_frame_number, page_frame_number_clean);
    }

    return page_frame_number != page_frame_number_clean;
  }

 private:
  // Return the image location, stripped of any directories, e.g. "boot.art" or "core.art"
  std::string GetImageLocationBaseName() const {
    return BaseName(std::string(image_location_));
  }

  std::ostream* os_;
  const ImageHeader& image_header_;
  const std::string image_location_;
  pid_t image_diff_pid_;  // Dump image diff against boot.art if pid is non-negative
  pid_t zygote_diff_pid_;  // Dump image diff against zygote boot.art if pid is non-negative

  DISALLOW_COPY_AND_ASSIGN(ImgDiagDumper);
};

static int DumpImage(Runtime* runtime,
                     std::ostream* os,
                     pid_t image_diff_pid,
                     pid_t zygote_diff_pid) {
  ScopedObjectAccess soa(Thread::Current());
  gc::Heap* heap = runtime->GetHeap();
  std::vector<gc::space::ImageSpace*> image_spaces = heap->GetBootImageSpaces();
  CHECK(!image_spaces.empty());
  for (gc::space::ImageSpace* image_space : image_spaces) {
    const ImageHeader& image_header = image_space->GetImageHeader();
    if (!image_header.IsValid()) {
      fprintf(stderr, "Invalid image header %s\n", image_space->GetImageLocation().c_str());
      return EXIT_FAILURE;
    }

    ImgDiagDumper img_diag_dumper(os,
                                  image_header,
                                  image_space->GetImageLocation(),
                                  image_diff_pid,
                                  zygote_diff_pid);
    if (!img_diag_dumper.Dump()) {
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}

struct ImgDiagArgs : public CmdlineArgs {
 protected:
  using Base = CmdlineArgs;

  virtual ParseStatus ParseCustom(const StringPiece& option,
                                  std::string* error_msg) OVERRIDE {
    {
      ParseStatus base_parse = Base::ParseCustom(option, error_msg);
      if (base_parse != kParseUnknownArgument) {
        return base_parse;
      }
    }

    if (option.starts_with("--image-diff-pid=")) {
      const char* image_diff_pid = option.substr(strlen("--image-diff-pid=")).data();

      if (!ParseInt(image_diff_pid, &image_diff_pid_)) {
        *error_msg = "Image diff pid out of range";
        return kParseError;
      }
    } else if (option.starts_with("--zygote-diff-pid=")) {
      const char* zygote_diff_pid = option.substr(strlen("--zygote-diff-pid=")).data();

      if (!ParseInt(zygote_diff_pid, &zygote_diff_pid_)) {
        *error_msg = "Zygote diff pid out of range";
        return kParseError;
      }
    } else {
      return kParseUnknownArgument;
    }

    return kParseOk;
  }

  virtual ParseStatus ParseChecks(std::string* error_msg) OVERRIDE {
    // Perform the parent checks.
    ParseStatus parent_checks = Base::ParseChecks(error_msg);
    if (parent_checks != kParseOk) {
      return parent_checks;
    }

    // Perform our own checks.

    if (kill(image_diff_pid_,
             /*sig*/0) != 0) {  // No signal is sent, perform error-checking only.
      // Check if the pid exists before proceeding.
      if (errno == ESRCH) {
        *error_msg = "Process specified does not exist";
      } else {
        *error_msg = StringPrintf("Failed to check process status: %s", strerror(errno));
      }
      return kParseError;
    } else if (instruction_set_ != kRuntimeISA) {
      // Don't allow different ISAs since the images are ISA-specific.
      // Right now the code assumes both the runtime ISA and the remote ISA are identical.
      *error_msg = "Must use the default runtime ISA; changing ISA is not supported.";
      return kParseError;
    }

    return kParseOk;
  }

  virtual std::string GetUsage() const {
    std::string usage;

    usage +=
        "Usage: imgdiag [options] ...\n"
        "    Example: imgdiag --image-diff-pid=$(pidof dex2oat)\n"
        "    Example: adb shell imgdiag --image-diff-pid=$(pid zygote)\n"
        "\n";

    usage += Base::GetUsage();

    usage +=  // Optional.
        "  --image-diff-pid=<pid>: provide the PID of a process whose boot.art you want to diff.\n"
        "      Example: --image-diff-pid=$(pid zygote)\n"
        "  --zygote-diff-pid=<pid>: provide the PID of the zygote whose boot.art you want to diff "
        "against.\n"
        "      Example: --zygote-diff-pid=$(pid zygote)\n"
        "\n";

    return usage;
  }

 public:
  pid_t image_diff_pid_ = -1;
  pid_t zygote_diff_pid_ = -1;
};

struct ImgDiagMain : public CmdlineMain<ImgDiagArgs> {
  virtual bool ExecuteWithRuntime(Runtime* runtime) {
    CHECK(args_ != nullptr);

    return DumpImage(runtime,
                     args_->os_,
                     args_->image_diff_pid_,
                     args_->zygote_diff_pid_) == EXIT_SUCCESS;
  }
};

}  // namespace art

int main(int argc, char** argv) {
  art::ImgDiagMain main;
  return main.Main(argc, argv);
}
