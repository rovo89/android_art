/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "UniquePtr.h"
#include "class_linker.h"
#include "dex_verifier.h"
#include "object.h"
#include "object_utils.h"
#include "jni.h"

namespace art {

#define REG(mh, reg_bitmap, reg) \
  ( ((reg) < mh.GetCodeItem()->registers_size_) &&                       \
    (( *((reg_bitmap) + (reg)/8) >> ((reg) % 8) ) & 0x01) )

#define CHECK_REGS(...) do {          \
    int t[] = {__VA_ARGS__};             \
    int t_size = sizeof(t) / sizeof(*t);      \
    for (int i = 0; i < t_size; ++i)          \
      CHECK(REG(mh, reg_bitmap, t[i])) << "Error: Reg " << i << " is not in RegisterMap";  \
  } while(false)

static int gJava_StackWalk_refmap_calls = 0;

struct ReferenceMapVisitor : public Thread::StackVisitor {
  ReferenceMapVisitor() {
  }

  void VisitFrame(const Frame& frame, uintptr_t pc) {
    Method* m = frame.GetMethod();
    CHECK(m != NULL);
    LOG(INFO) << "At " << PrettyMethod(m, false);

    if (m->IsCalleeSaveMethod() || m->IsNative()) {
      LOG(WARNING) << "no PC for " << PrettyMethod(m);
      CHECK_EQ(pc, 0u);
      return;
    }
    verifier::PcToReferenceMap map(m->GetGcMap(), m->GetGcMapLength());
    const uint8_t* reg_bitmap = map.FindBitMap(m->ToDexPC(pc));
    MethodHelper mh(m);
    StringPiece m_name(mh.GetName());

    // Given the method name and the number of times the method has been called,
    // we know the Dex registers with live reference values. Assert that what we
    // find is what is expected.
    if (m_name == "f") {
      if (gJava_StackWalk_refmap_calls == 1) {
        CHECK_EQ(1U, m->ToDexPC(pc));
        CHECK_REGS(1);
      } else {
        CHECK_EQ(gJava_StackWalk_refmap_calls, 2);
        CHECK_EQ(5U, m->ToDexPC(pc));
        CHECK_REGS(1);
      }
    } else if (m_name == "g") {
      if (gJava_StackWalk_refmap_calls == 1) {
        CHECK_EQ(0xcU, m->ToDexPC(pc));
        CHECK_REGS(0, 2);  // Note that v1 is not in the minimal root set
      } else {
        CHECK_EQ(gJava_StackWalk_refmap_calls, 2);
        CHECK_EQ(0xcU, m->ToDexPC(pc));
        CHECK_REGS(0, 2);
      }
    } else if (m_name == "shlemiel") {
      if (gJava_StackWalk_refmap_calls == 1) {
        CHECK_EQ(0x380U, m->ToDexPC(pc));
        CHECK_REGS(2, 4, 5, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 21, 25);
      } else {
        CHECK_EQ(gJava_StackWalk_refmap_calls, 2);
        CHECK_EQ(0x380U, m->ToDexPC(pc));
        CHECK_REGS(2, 4, 5, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 21, 25);
      }
    }
    LOG(INFO) << reinterpret_cast<const void*>(reg_bitmap);
  }
};

extern "C"
JNIEXPORT jint JNICALL Java_StackWalk_refmap(JNIEnv* env, jobject thisObj, jint count) {
  CHECK_EQ(count, 0);
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
