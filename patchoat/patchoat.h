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

#ifndef ART_PATCHOAT_PATCHOAT_H_
#define ART_PATCHOAT_PATCHOAT_H_

#include "arch/instruction_set.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "elf_file.h"
#include "elf_utils.h"
#include "gc/accounting/space_bitmap.h"
#include "gc/heap.h"
#include "os.h"

namespace art {

class ArtMethod;
class ImageHeader;
class OatHeader;

namespace mirror {
class Object;
class PointerArray;
class Reference;
class Class;
}  // namespace mirror

class PatchOat {
 public:
  // Patch only the oat file
  static bool Patch(File* oat_in, off_t delta, File* oat_out, TimingLogger* timings,
                    bool output_oat_opened_from_fd,  // Was this using --oatput-oat-fd ?
                    bool new_oat_out);               // Output oat was a new file created by us?

  // Patch only the image (art file)
  static bool Patch(const std::string& art_location, off_t delta, File* art_out, InstructionSet isa,
                    TimingLogger* timings);

  // Patch both the image and the oat file
  static bool Patch(File* oat_in, const std::string& art_location,
                    off_t delta, File* oat_out, File* art_out, InstructionSet isa,
                    TimingLogger* timings,
                    bool output_oat_opened_from_fd,  // Was this using --oatput-oat-fd ?
                    bool new_oat_out);               // Output oat was a new file created by us?

 private:
  // Takes ownership only of the ElfFile. All other pointers are only borrowed.
  PatchOat(ElfFile* oat_file, off_t delta, TimingLogger* timings)
      : oat_file_(oat_file), image_(nullptr), bitmap_(nullptr), heap_(nullptr), delta_(delta),
        isa_(kNone), timings_(timings) {}
  PatchOat(InstructionSet isa, MemMap* image, gc::accounting::ContinuousSpaceBitmap* bitmap,
           MemMap* heap, off_t delta, TimingLogger* timings)
      : image_(image), bitmap_(bitmap), heap_(heap),
        delta_(delta), isa_(isa), timings_(timings) {}
  PatchOat(InstructionSet isa, ElfFile* oat_file, MemMap* image,
           gc::accounting::ContinuousSpaceBitmap* bitmap, MemMap* heap, off_t delta,
           TimingLogger* timings)
      : oat_file_(oat_file), image_(image), bitmap_(bitmap), heap_(heap),
        delta_(delta), isa_(isa), timings_(timings) {}
  ~PatchOat() {}

  // Was the .art image at image_path made with --compile-pic ?
  static bool IsImagePic(const ImageHeader& image_header, const std::string& image_path);

  enum MaybePic {
      NOT_PIC,            // Code not pic. Patch as usual.
      PIC,                // Code was pic. Create symlink; skip OAT patching.
      ERROR_OAT_FILE,     // Failed to symlink oat file
      ERROR_FIRST = ERROR_OAT_FILE,
  };

  // Was the .oat image at oat_in made with --compile-pic ?
  static MaybePic IsOatPic(const ElfFile* oat_in);

  // Attempt to replace the file with a symlink
  // Returns false if it fails
  static bool ReplaceOatFileWithSymlink(const std::string& input_oat_filename,
                                        const std::string& output_oat_filename,
                                        bool output_oat_opened_from_fd,
                                        bool new_oat_out);  // Output oat was newly created?

  static void BitmapCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    reinterpret_cast<PatchOat*>(arg)->VisitObject(obj);
  }

  void VisitObject(mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void FixupMethod(ArtMethod* object, ArtMethod* copy)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void FixupNativePointerArray(mirror::PointerArray* object)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool InHeap(mirror::Object*);

  // Patches oat in place, modifying the oat_file given to the constructor.
  bool PatchElf();
  template <typename ElfFileImpl>
  bool PatchElf(ElfFileImpl* oat_file);
  template <typename ElfFileImpl>
  bool PatchOatHeader(ElfFileImpl* oat_file);

  bool PatchImage() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void PatchArtFields(const ImageHeader* image_header) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void PatchArtMethods(const ImageHeader* image_header) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void PatchInternedStrings(const ImageHeader* image_header)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void PatchDexFileArrays(mirror::ObjectArray<mirror::Object>* img_roots)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool WriteElf(File* out);
  bool WriteImage(File* out);

  template <typename T>
  T* RelocatedCopyOf(T* obj) const {
    if (obj == nullptr) {
      return nullptr;
    }
    DCHECK_GT(reinterpret_cast<uintptr_t>(obj), reinterpret_cast<uintptr_t>(heap_->Begin()));
    DCHECK_LT(reinterpret_cast<uintptr_t>(obj), reinterpret_cast<uintptr_t>(heap_->End()));
    uintptr_t heap_off =
        reinterpret_cast<uintptr_t>(obj) - reinterpret_cast<uintptr_t>(heap_->Begin());
    DCHECK_LT(heap_off, image_->Size());
    return reinterpret_cast<T*>(image_->Begin() + heap_off);
  }

  template <typename T>
  T* RelocatedAddressOfPointer(T* obj) const {
    if (obj == nullptr) {
      return obj;
    }
    auto ret = reinterpret_cast<uintptr_t>(obj) + delta_;
    // Trim off high bits in case negative relocation with 64 bit patchoat.
    if (InstructionSetPointerSize(isa_) == sizeof(uint32_t)) {
      ret = static_cast<uintptr_t>(static_cast<uint32_t>(ret));
    }
    return reinterpret_cast<T*>(ret);
  }

  template <typename T>
  T RelocatedAddressOfIntPointer(T obj) const {
    if (obj == 0) {
      return obj;
    }
    T ret = obj + delta_;
    // Trim off high bits in case negative relocation with 64 bit patchoat.
    if (InstructionSetPointerSize(isa_) == 4) {
      ret = static_cast<T>(static_cast<uint32_t>(ret));
    }
    return ret;
  }

  // Look up the oat header from any elf file.
  static const OatHeader* GetOatHeader(const ElfFile* elf_file);

  // Templatized version to actually look up the oat header
  template <typename ElfFileImpl>
  static const OatHeader* GetOatHeader(const ElfFileImpl* elf_file);

  // Walks through the old image and patches the mmap'd copy of it to the new offset. It does not
  // change the heap.
  class PatchVisitor {
  public:
    PatchVisitor(PatchOat* patcher, mirror::Object* copy) : patcher_(patcher), copy_(copy) {}
    ~PatchVisitor() {}
    void operator() (mirror::Object* obj, MemberOffset off, bool b) const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_);
    // For reference classes.
    void operator() (mirror::Class* cls, mirror::Reference* ref) const
      EXCLUSIVE_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_);
  private:
    PatchOat* const patcher_;
    mirror::Object* const copy_;
  };

  // The elf file we are patching.
  std::unique_ptr<ElfFile> oat_file_;
  // A mmap of the image we are patching. This is modified.
  const MemMap* const image_;
  // The bitmap over the image within the heap we are patching. This is not modified.
  gc::accounting::ContinuousSpaceBitmap* const bitmap_;
  // The heap we are patching. This is not modified.
  const MemMap* const heap_;
  // The amount we are changing the offset by.
  const off_t delta_;
  // Active instruction set, used to know the entrypoint size.
  const InstructionSet isa_;

  TimingLogger* timings_;

  friend class FixupRootVisitor;
  DISALLOW_IMPLICIT_CONSTRUCTORS(PatchOat);
};

}  // namespace art
#endif  // ART_PATCHOAT_PATCHOAT_H_
