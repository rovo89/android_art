// Copyright 2011 Google Inc. All Rights Reserved.

#include "common_test.h"

namespace art {

class HeapTest : public CommonTest {};

TEST_F(HeapTest, GarbageCollectClassLinkerInit) {
  // garbage is created during ClassLinker::Init

  Class* c = class_linker_->FindSystemClass("[Ljava/lang/Object;");
  for (size_t i = 0; i < 1024; ++i) {
    SirtRef<ObjectArray<Object> > array(ObjectArray<Object>::Alloc(c, 2048));
    for (size_t j = 0; j < 2048; ++j) {
      array->Set(j, String::AllocFromModifiedUtf8("hello, world!"));
    }
  }

  Heap::CollectGarbage();
}

}  // namespace art
