#include "heap_bitmap.h"

namespace art {

void HeapBitmap::ReplaceBitmap(SpaceBitmap* old_bitmap, SpaceBitmap* new_bitmap) {
  // TODO: C++0x auto
  for (Bitmaps::iterator cur  = bitmaps_.begin(); cur != bitmaps_.end(); ++cur) {
    if (*cur == old_bitmap) {
      *cur = new_bitmap;
      return;
    }
  }
  LOG(FATAL) << "bitmap " << static_cast<const void*>(old_bitmap) << " not found";
}

}  // namespace art
