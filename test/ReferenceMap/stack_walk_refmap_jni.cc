// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdio.h>

#include "UniquePtr.h"
#include "class_linker.h"
#include "dex_verifier.h"
#include "object.h"
#include "jni.h"

namespace art {

#define IS_IN_REF_BITMAP(method, ref_bitmap, reg) \
  ( ((reg) < (method)->NumRegisters()) &&                       \
    (( *((ref_bitmap) + (reg)/8) >> ((reg) % 8) ) & 0x01) )

#define CHECK_REGS_CONTAIN_REFS(...)     \
  do {                                   \
    int t[] = {__VA_ARGS__};             \
    int t_size = sizeof(t) / sizeof(*t);      \
    for (int i = 0; i < t_size; ++i)          \
      CHECK(IS_IN_REF_BITMAP(m, ref_bitmap, t[i])) \
          << "Error: Reg @ " << i << "-th argument is not in GC map"; \
  } while(false)

struct ReferenceMap2Visitor : public Thread::StackVisitor {
  ReferenceMap2Visitor() {
  }

  void VisitFrame(const Frame& frame, uintptr_t pc) {
    Method* m = frame.GetMethod();
    if (!m || m->IsNative()) {
      return;
    }
    LOG(INFO) << "At " << PrettyMethod(m, false);

    verifier::PcToReferenceMap map(m);

    if (!pc) {
      // pc == NULL: m is either a native method or a phony method
      return;
    }
    if (m->IsCalleeSaveMethod()) {
      LOG(WARNING) << "no PC for " << PrettyMethod(m);
      return;
    }

    const uint8_t* ref_bitmap = NULL;
    std::string m_name = m->GetName()->ToModifiedUtf8();

    // Given the method name and the number of times the method has been called,
    // we know the Dex registers with live reference values. Assert that what we
    // find is what is expected.
    if (m_name.compare("f") == 0) {
      ref_bitmap = map.FindBitMap(0x03U);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8);  // v8: this

      ref_bitmap = map.FindBitMap(0x06U);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 1);  // v7: this, v2: x

      ref_bitmap = map.FindBitMap(0x08U);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 3, 1);  // v7: this, v2: x, v0: y

      ref_bitmap = map.FindBitMap(0x0cU);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 3, 1);  // v7: this, v2: x, v0: y

      ref_bitmap = map.FindBitMap(0x0eU);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 3, 1);  // v7: this, v2: x, v0: y

      ref_bitmap = map.FindBitMap(0x10U);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 3, 1);  // v7: this, v2: x, v0: y

      ref_bitmap = map.FindBitMap(0x13U);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 3, 2, 1);  // v7: this, v2: x, v0: y

      ref_bitmap = map.FindBitMap(0x15U);
      CHECK(ref_bitmap);
        // FIXME: v1?
      CHECK_REGS_CONTAIN_REFS(8, 2, 1, 0);  // v7: this, v2: x, v0: y, v1: y or ex.

      ref_bitmap = map.FindBitMap(0x18U);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 2, 1, 0);  // v7: this, v2: x, v0: y

      ref_bitmap = map.FindBitMap(0x1aU);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 5, 2, 1, 0);  // v7: this, v2: x, v0: y, v3: x[1]

      ref_bitmap = map.FindBitMap(0x1dU);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 5, 2, 1, 0);  // v7: this, v2: x, v0: y, v3: x[1]

      ref_bitmap = map.FindBitMap(0x1fU);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 2, 1, 0);  // v7: this, v2: x, v0: y

      ref_bitmap = map.FindBitMap(0x21U);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 2, 1, 0);  // v7: this, v2: x, v0: y

      ref_bitmap = map.FindBitMap(0x25U);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 3, 2, 1, 0);  // v7: this, v2: x, v0: y

      ref_bitmap = map.FindBitMap(0x27U);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 4, 2, 1);  // v7: this, v2: x, v0: ex, v1: y

      ref_bitmap = map.FindBitMap(0x29U);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 4, 2, 1);  // v7: this, v2: x, v0: ex, v1: y

      ref_bitmap = map.FindBitMap(0x2cU);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 4, 2, 1);  // v7: this, v2: x, v0: ex, v1: y

      ref_bitmap = map.FindBitMap(0x2fU);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 4, 3, 2, 1);  // v7: this, v2: x, v0: ex, v1: y, v6: ex

      ref_bitmap = map.FindBitMap(0x32U);
      CHECK(ref_bitmap);
      CHECK_REGS_CONTAIN_REFS(8, 3, 2, 1, 0);  // v7: this, v2: x, v0: ex, v1: y
    }

  }
};

extern "C"
JNIEXPORT jint JNICALL Java_ReferenceMap_refmap(JNIEnv* env, jobject thisObj, jint count) {
  // Visitor
  ReferenceMap2Visitor mapper;
  Thread::Current()->WalkStack(&mapper);

  return count + 1;
}

}
