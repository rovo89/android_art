// Copyright 2011 Google Inc. All Rights Reserved.

#include <stdio.h>

#include "UniquePtr.h"
#include "class_linker.h"
#include "common_test.h"
#include "dex_verifier.h"
#include "object.h"
#include "jni.h"

namespace art {

#define REG(method, reg_vector, reg) \
  ( ((reg) < (method)->NumRegisters()) &&                       \
    (( *((reg_vector) + (reg)/8) >> ((reg) % 8) ) & 0x01) )

#define EXPECT_REGS(...) do {          \
    int t[] = {__VA_ARGS__};             \
    int t_size = sizeof(t) / sizeof(*t);      \
    for (int i = 0; i < t_size; ++i)          \
      EXPECT_TRUE(REG(m, reg_vector, t[i]));  \
  } while(false)

static int gJava_StackWalk_refmap_calls = 0;

struct ReferenceMapVisitor : public Thread::StackVisitor {
  ReferenceMapVisitor() {
  }

  void VisitFrame(const Frame& frame, uintptr_t pc) {
    Method* m = frame.GetMethod();
    if (!m ||m->IsNative()) {
      return;
    }
    LOG(INFO) << "At " << PrettyMethod(m, false);

    art::DexVerifier::RegisterMap* map = new art::DexVerifier::RegisterMap(
        m->GetRegisterMapHeader(),
        m->GetRegisterMapData());

    if (!pc) {
      // pc == NULL: m is either a native method or a phony method
      return;
    }
    if (m->IsCalleeSaveMethod()) {
      LOG(WARNING) << "no PC for " << PrettyMethod(m);
      return;
    }

    const uint8_t* reg_vector = art::DexVerifier::RegisterMapGetLine(map, m->ToDexPC(pc));
    std::string m_name = m->GetName()->ToModifiedUtf8();

    // Given the method name and the number of times the method has been called,
    // we know the Dex registers with live reference values. Assert that what we
    // find is what is expected.
    if (m_name.compare("f") == 0) {
      if (gJava_StackWalk_refmap_calls == 1) {
        EXPECT_EQ(0U, m->ToDexPC(pc));
        EXPECT_REGS(1);
      } else {
        CHECK_EQ(gJava_StackWalk_refmap_calls, 2);
        EXPECT_EQ(4U, m->ToDexPC(pc));
        EXPECT_REGS(1);  // Note that v1 is not in the minimal root set
      }
    } else if (m_name.compare("g") == 0) {
      if (gJava_StackWalk_refmap_calls == 1) {
        EXPECT_EQ(0xaU, m->ToDexPC(pc));
        EXPECT_REGS(1, 2, 3);
      } else {
        CHECK_EQ(gJava_StackWalk_refmap_calls, 2);
        EXPECT_EQ(0xaU, m->ToDexPC(pc));
          EXPECT_REGS(1, 2, 3);
      }
    } else if (m_name.compare("shlemiel") == 0) {
      if (gJava_StackWalk_refmap_calls == 1) {
        EXPECT_EQ(0x2d5U, m->ToDexPC(pc));
        EXPECT_REGS(2, 4, 5, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 21, 25);
      } else {
        CHECK_EQ(gJava_StackWalk_refmap_calls, 2);
        EXPECT_EQ(0x2d5U, m->ToDexPC(pc));
        EXPECT_REGS(2, 4, 5, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 21, 25);
      }
    }

    LOG(INFO) << reg_vector;
  }
};

extern "C"
JNIEXPORT jint JNICALL Java_StackWalk_refmap(JNIEnv* env, jobject thisObj, jint count) {
  EXPECT_EQ(count, 0);
  gJava_StackWalk_refmap_calls++;

  // Visitor
  ReferenceMapVisitor mapper;
  Thread::Current()->WalkStack(&mapper);

  return count + 1;
}

extern "C"
JNIEXPORT jint JNICALL Java_StackWalk2_refmap2(JNIEnv* env, jobject thisObj, jint count) {
  gJava_StackWalk_refmap_calls++;

  // Visitor
  ReferenceMapVisitor mapper;
  Thread::Current()->WalkStack(&mapper);

  return count + 1;
}

}
