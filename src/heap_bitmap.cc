#include "heap_bitmap.h"
#include "space.h"

namespace art {

void HeapBitmap::ReplaceBitmap(SpaceBitmap* old_bitmap, SpaceBitmap* new_bitmap) {
  // TODO: C++0x auto
  for (Bitmaps::iterator it = bitmaps_.begin(); it != bitmaps_.end(); ++it) {
    if (*it == old_bitmap) {
      *it = new_bitmap;
      return;
    }
  }
  LOG(FATAL) << "bitmap " << static_cast<const void*>(old_bitmap) << " not found";
}

void HeapBitmap::AddSpaceBitmap(SpaceBitmap* bitmap) {
  DCHECK(bitmap != NULL);

  // Check for interval overlap.
  for (Bitmaps::const_iterator it = bitmaps_.begin(); it != bitmaps_.end(); ++it) {
    SpaceBitmap* cur_bitmap = *it;
    if (bitmap->HeapBegin() < cur_bitmap->HeapSize() + cur_bitmap->HeapSize() &&
        bitmap->HeapBegin() + bitmap->HeapSize() > cur_bitmap->HeapBegin()) {
      LOG(FATAL) << "Overlapping space bitmaps added to heap bitmap!";
    }
  }
  bitmaps_.push_back(bitmap);
}

}  // namespace art
