/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "common_runtime_test.h"
#include "gc/collector/immune_spaces.h"
#include "gc/space/image_space.h"
#include "gc/space/space-inl.h"
#include "oat_file.h"
#include "thread-inl.h"

namespace art {
namespace mirror {
class Object;
}  // namespace mirror
namespace gc {
namespace collector {

class ImmuneSpacesTest : public CommonRuntimeTest {};

class DummySpace : public space::ContinuousSpace {
 public:
  DummySpace(uint8_t* begin, uint8_t* end)
      : ContinuousSpace("DummySpace",
                        space::kGcRetentionPolicyNeverCollect,
                        begin,
                        end,
                        /*limit*/end) {}

  space::SpaceType GetType() const OVERRIDE {
    return space::kSpaceTypeMallocSpace;
  }

  bool CanMoveObjects() const OVERRIDE {
    return false;
  }

  accounting::ContinuousSpaceBitmap* GetLiveBitmap() const OVERRIDE {
    return nullptr;
  }

  accounting::ContinuousSpaceBitmap* GetMarkBitmap() const OVERRIDE {
    return nullptr;
  }
};

TEST_F(ImmuneSpacesTest, AppendBasic) {
  ImmuneSpaces spaces;
  uint8_t* const base = reinterpret_cast<uint8_t*>(0x1000);
  DummySpace a(base, base + 45 * KB);
  DummySpace b(a.Limit(), a.Limit() + 813 * KB);
  {
    WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    spaces.AddSpace(&a);
    spaces.AddSpace(&b);
  }
  EXPECT_TRUE(spaces.ContainsSpace(&a));
  EXPECT_TRUE(spaces.ContainsSpace(&b));
  EXPECT_EQ(reinterpret_cast<uint8_t*>(spaces.GetLargestImmuneRegion().Begin()), a.Begin());
  EXPECT_EQ(reinterpret_cast<uint8_t*>(spaces.GetLargestImmuneRegion().End()), b.Limit());
}

class DummyOatFile : public OatFile {
 public:
  DummyOatFile(uint8_t* begin, uint8_t* end) : OatFile("Location", /*is_executable*/ false) {
    begin_ = begin;
    end_ = end;
  }
};

class DummyImageSpace : public space::ImageSpace {
 public:
  DummyImageSpace(MemMap* map,
                  accounting::ContinuousSpaceBitmap* live_bitmap,
                  std::unique_ptr<DummyOatFile>&& oat_file)
      : ImageSpace("DummyImageSpace",
                   /*image_location*/"",
                   map,
                   live_bitmap,
                   map->End()) {
    oat_file_ = std::move(oat_file);
    oat_file_non_owned_ = oat_file_.get();
  }

  // Size is the size of the image space, oat offset is where the oat file is located
  // after the end of image space. oat_size is the size of the oat file.
  static DummyImageSpace* Create(size_t size, size_t oat_offset, size_t oat_size) {
    std::string error_str;
    std::unique_ptr<MemMap> map(MemMap::MapAnonymous("DummyImageSpace",
                                                     nullptr,
                                                     size,
                                                     PROT_READ | PROT_WRITE,
                                                     /*low_4gb*/true,
                                                     /*reuse*/false,
                                                     &error_str));
    if (map == nullptr) {
      LOG(ERROR) << error_str;
      return nullptr;
    }
    std::unique_ptr<accounting::ContinuousSpaceBitmap> live_bitmap(
        accounting::ContinuousSpaceBitmap::Create("bitmap", map->Begin(), map->Size()));
    if (live_bitmap == nullptr) {
      return nullptr;
    }
    // The actual mapped oat file may not be directly after the image for the app image case.
    std::unique_ptr<DummyOatFile> oat_file(new DummyOatFile(map->End() + oat_offset,
                                                            map->End() + oat_offset + oat_size));
    // Create image header.
    ImageSection sections[ImageHeader::kSectionCount];
    new (map->Begin()) ImageHeader(
        /*image_begin*/PointerToLowMemUInt32(map->Begin()),
        /*image_size*/map->Size(),
        sections,
        /*image_roots*/PointerToLowMemUInt32(map->Begin()) + 1,
        /*oat_checksum*/0u,
        // The oat file data in the header is always right after the image space.
        /*oat_file_begin*/PointerToLowMemUInt32(map->End()),
        /*oat_data_begin*/PointerToLowMemUInt32(map->End()),
        /*oat_data_end*/PointerToLowMemUInt32(map->End() + oat_size),
        /*oat_file_end*/PointerToLowMemUInt32(map->End() + oat_size),
        /*boot_image_begin*/0u,
        /*boot_image_size*/0u,
        /*boot_oat_begin*/0u,
        /*boot_oat_size*/0u,
        /*pointer_size*/sizeof(void*),
        /*compile_pic*/false,
        /*is_pic*/false,
        ImageHeader::kStorageModeUncompressed,
        /*storage_size*/0u);
    return new DummyImageSpace(map.release(), live_bitmap.release(), std::move(oat_file));
  }
};

TEST_F(ImmuneSpacesTest, AppendAfterImage) {
  ImmuneSpaces spaces;
  constexpr size_t image_size = 123 * kPageSize;
  constexpr size_t image_oat_size = 321 * kPageSize;
  std::unique_ptr<DummyImageSpace> image_space(DummyImageSpace::Create(image_size,
                                                                       0,
                                                                       image_oat_size));
  ASSERT_TRUE(image_space != nullptr);
  const ImageHeader& image_header = image_space->GetImageHeader();
  EXPECT_EQ(image_header.GetImageSize(), image_size);
  EXPECT_EQ(static_cast<size_t>(image_header.GetOatFileEnd() - image_header.GetOatFileBegin()),
            image_oat_size);
  DummySpace space(image_header.GetOatFileEnd(), image_header.GetOatFileEnd() + 813 * kPageSize);
  EXPECT_NE(image_space->Limit(), space.Begin());
  {
    WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    spaces.AddSpace(image_space.get());
    spaces.AddSpace(&space);
  }
  EXPECT_TRUE(spaces.ContainsSpace(image_space.get()));
  EXPECT_TRUE(spaces.ContainsSpace(&space));
  // CreateLargestImmuneRegion should have coalesced the two spaces since the oat code after the
  // image prevents gaps.
  // Check that we have a continuous region.
  EXPECT_EQ(reinterpret_cast<uint8_t*>(spaces.GetLargestImmuneRegion().Begin()),
            image_space->Begin());
  EXPECT_EQ(reinterpret_cast<uint8_t*>(spaces.GetLargestImmuneRegion().End()), space.Limit());
  // Check that appending with a gap between the map does not include the oat file.
  image_space.reset(DummyImageSpace::Create(image_size, kPageSize, image_oat_size));
  spaces.Reset();
  {
    WriterMutexLock mu(Thread::Current(), *Locks::heap_bitmap_lock_);
    spaces.AddSpace(image_space.get());
  }
  EXPECT_EQ(reinterpret_cast<uint8_t*>(spaces.GetLargestImmuneRegion().Begin()),
            image_space->Begin());
  // Size should be equal, we should not add the oat file since it is not adjacent to the image
  // space.
  EXPECT_EQ(spaces.GetLargestImmuneRegion().Size(), image_size);
}

}  // namespace collector
}  // namespace gc
}  // namespace art
