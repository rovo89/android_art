// Copyright 2011 Google Inc. All Rights Reserved.

#include "common_test.h"

namespace art {

class HeapTest : public CommonTest {};

TEST_F(HeapTest, GarbageCollectClassLinkerInit) {
  // garbage is created during ClassLinker::Init
  Heap::CollectGarbage();
}

} // namespace art
