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

#include "base/macros.h"
#include "base/mutex.h"
#include "instruction_set.h"
#include "os.h"
#include "elf_file.h"
#include "elf_utils.h"
#include "gc/accounting/space_bitmap.h"
#include "gc/heap.h"
#include "utils.h"

namespace art {

class ImageHeader;

namespace mirror {
class Object;
class Reference;
class Class;
class ArtMethod;
}  // namespace mirror

class PatchOat {
 public:
  static bool Patch(File* oat_in, off_t delta, File* oat_out, TimingLogger* timings);

  static bool Patch(const std::string& art_location, off_t delta, File* art_out, InstructionSet isa,
                    TimingLogger* timings);

  static bool Patch(const File* oat_in, const std::string& art_location,
                    off_t delta, File* oat_out, File* art_out, InstructionSet isa,
                    TimingLogger* timings);

 private:
  // Takes ownership only of the ElfFile. All other pointers are only borrowed.
  PatchOat(ElfFile* oat_file, off_t delta, TimingLogger* timings)
      : oat_file_(oat_file), image_(nullptr), bitmap_(nullptr), heap_(nullptr), delta_(delta),
        timings_(timings) {}
  PatchOat(MemMap* image, gc::accounting::ContinuousSpaceBitmap* bitmap,
           MemMap* heap, off_t delta, TimingLogger* timings)
      : image_(image), bitmap_(bitmap), heap_(heap),
        delta_(delta), timings_(timings) {}
  PatchOat(ElfFile* oat_file, MemMap* image, gc::accounting::ContinuousSpaceBitmap* bitmap,
           MemMap* heap, off_t delta, TimingLogger* timings)
      : oat_file_(oat_file), image_(image), bitmap_(bitmap), heap_(heap),
        delta_(delta), timings_(timings) {}
  ~PatchOat() {}

  static void BitmapCallback(mirror::Object* obj, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    reinterpret_cast<PatchOat*>(arg)->VisitObject(obj);
  }

  void VisitObject(mirror::Object* obj)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  void FixupMethod(mirror::ArtMethod* object, mirror::ArtMethod* copy)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  bool InHeap(mirror::Object*);

  // Patches oat in place, modifying the oat_file given to the constructor.
  bool PatchElf();
  template <typename ElfFileImpl>
  bool PatchElf(ElfFileImpl* oat_file);
  template <typename ElfFileImpl>
  bool PatchTextSection(ElfFileImpl* oat_file);
  // Templatized version to actually do the patching with the right sized offsets.
  template <typename ElfFileImpl, typename patch_loc_t> bool PatchTextSection(ElfFileImpl* oat_file);
  template <typename ElfFileImpl, typename patch_loc_t> bool CheckOatFile(ElfFileImpl* oat_filec);
  template <typename ElfFileImpl>
  bool PatchOatHeader(ElfFileImpl* oat_file);

  bool PatchImage() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool WriteElf(File* out);
  bool WriteImage(File* out);

  mirror::Object* RelocatedCopyOf(mirror::Object*);
  mirror::Object* RelocatedAddressOf(mirror::Object* obj);

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
  // Timing splits.
  TimingLogger* const timings_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(PatchOat);
};

}  // namespace art
#endif  // ART_PATCHOAT_PATCHOAT_H_
