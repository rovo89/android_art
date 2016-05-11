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

#include "art_method-inl.h"
#include "check_reference_map_visitor.h"
#include "jni.h"

namespace art {

#define CHECK_REGS_CONTAIN_REFS(dex_pc, abort_if_not_found, ...) do {                 \
  int t[] = {__VA_ARGS__};                                                            \
  int t_size = sizeof(t) / sizeof(*t);                                                \
  const OatQuickMethodHeader* method_header = GetCurrentOatQuickMethodHeader();       \
  uintptr_t native_quick_pc = method_header->ToNativeQuickPc(GetMethod(),             \
                                                 dex_pc,                              \
                                                 /* is_catch_handler */ false,        \
                                                 abort_if_not_found);                 \
  if (native_quick_pc != UINTPTR_MAX) {                                               \
    CheckReferences(t, t_size, method_header->NativeQuickPcOffset(native_quick_pc));  \
  }                                                                                   \
} while (false);

struct ReferenceMap2Visitor : public CheckReferenceMapVisitor {
  explicit ReferenceMap2Visitor(Thread* thread) SHARED_REQUIRES(Locks::mutator_lock_)
      : CheckReferenceMapVisitor(thread) {}

  bool VisitFrame() SHARED_REQUIRES(Locks::mutator_lock_) {
    if (CheckReferenceMapVisitor::VisitFrame()) {
      return true;
    }
    ArtMethod* m = GetMethod();
    std::string m_name(m->GetName());

    // Given the method name and the number of times the method has been called,
    // we know the Dex registers with live reference values. Assert that what we
    // find is what is expected.
    if (m_name.compare("f") == 0) {
      CHECK_REGS_CONTAIN_REFS(0x03U, true, 8);  // v8: this
      CHECK_REGS_CONTAIN_REFS(0x06U, true, 8, 1);  // v8: this, v1: x
      CHECK_REGS_CONTAIN_REFS(0x0cU, true, 8, 3, 1);  // v8: this, v3: y, v1: x
      CHECK_REGS_CONTAIN_REFS(0x10U, true, 8, 3, 1);  // v8: this, v3: y, v1: x
      // v2 is added because of the instruction at DexPC 0024. Object merges with 0 is Object. See:
      //   0024: move-object v3, v2
      //   0025: goto 0013
      // Detailed dex instructions for ReferenceMap.java are at the end of this function.
      // CHECK_REGS_CONTAIN_REFS(8, 3, 2, 1);  // v8: this, v3: y, v2: y, v1: x
      // We eliminate the non-live registers at a return, so only v3 is live.
      // Note that it is OK for a compiler to not have a dex map at this dex PC because
      // a return is not necessarily a safepoint.
      CHECK_REGS_CONTAIN_REFS(0x14U, false, 2);  // v2: y
      // Note that v0: ex can be eliminated because it's a dead merge of two different exceptions.
      CHECK_REGS_CONTAIN_REFS(0x18U, true, 8, 2, 1);  // v8: this, v2: y, v1: x (dead v0: ex)
      CHECK_REGS_CONTAIN_REFS(0x22U, true, 8, 2, 1);  // v8: this, v2: y, v1: x (dead v0: ex)

      if (!GetCurrentOatQuickMethodHeader()->IsOptimized()) {
        CHECK_REGS_CONTAIN_REFS(0x27U, true, 8, 4, 2, 1);  // v8: this, v4: ex, v2: y, v1: x
      }
      CHECK_REGS_CONTAIN_REFS(0x29U, true, 8, 4, 2, 1);  // v8: this, v4: ex, v2: y, v1: x
      CHECK_REGS_CONTAIN_REFS(0x2cU, true, 8, 4, 2, 1);  // v8: this, v4: ex, v2: y, v1: x
      // Note that it is OK for a compiler to not have a dex map at these two dex PCs because
      // a goto is not necessarily a safepoint.
      CHECK_REGS_CONTAIN_REFS(0x2fU, false, 8, 4, 3, 2, 1);  // v8: this, v4: ex, v3: y, v2: y, v1: x
      CHECK_REGS_CONTAIN_REFS(0x32U, false, 8, 3, 2, 1, 0);  // v8: this, v3: y, v2: y, v1: x, v0: ex
    }

    return true;
  }
};

// DEX code
//
// 0000: const/4 v4, #int 2 // #2
// 0001: const/4 v7, #int 0 // #0
// 0002: const/4 v6, #int 1 // #1
// 0003: new-array v1, v4, [Ljava/lang/Object; // type@0007
// 0005: const/4 v2, #int 0 // #0
// 0006: new-instance v3, Ljava/lang/Object; // type@0003
// 0008: invoke-direct {v3}, Ljava/lang/Object;.<init>:()V // method@0004
// 000b: const/4 v4, #int 2 // #2
// 000c: aput-object v3, v1, v4
// 000e: aput-object v3, v1, v6
// 0010: invoke-virtual {v8, v7}, LMain;.refmap:(I)I // method@0003
// 0013: move-object v2, v3
// 0014: return-object v2
// 0015: move-exception v0
// 0016: if-nez v2, 0020 // +000a
// 0018: new-instance v4, Ljava/lang/Object; // type@0003
// 001a: invoke-direct {v4}, Ljava/lang/Object;.<init>:()V // method@0004
// 001d: const/4 v5, #int 1 // #1
// 001e: aput-object v4, v1, v5
// 0020: aput-object v2, v1, v6
// 0022: invoke-virtual {v8, v7}, LMain;.refmap:(I)I // method@0003
// 0025: goto 0014 // -0011
// 0026: move-exception v4
// 0027: aput-object v2, v1, v6
// 0029: invoke-virtual {v8, v7}, LMain;.refmap:(I)I // method@0003
// 002c: throw v4
// 002d: move-exception v4
// 002e: move-object v2, v3
// 002f: goto 0027 // -0008
// 0030: move-exception v0
// 0031: move-object v2, v3
// 0032: goto 0016 // -001c
//    catches       : 3
//      0x0006 - 0x000b
//        Ljava/lang/Exception; -> 0x0015
//        <any> -> 0x0026
//      0x000c - 0x000e
//        Ljava/lang/Exception; -> 0x0030
//        <any> -> 0x002d
//      0x0018 - 0x0020
//        <any> -> 0x0026
//    positions     :
//      0x0003 line=22
//      0x0005 line=23
//      0x0006 line=25
//      0x000b line=26
//      0x000e line=32
//      0x0010 line=33
//      0x0014 line=35
//      0x0015 line=27
//      0x0016 line=28
//      0x0018 line=29
//      0x0020 line=32
//      0x0022 line=33
//      0x0026 line=31
//      0x0027 line=32
//      0x0029 line=33
//      0x002c line=31
//      0x0030 line=27
//    locals        :
//      0x0006 - 0x000b reg=2 y Ljava/lang/Object;
//      0x000b - 0x0014 reg=3 y Ljava/lang/Object;
//      0x0015 - 0x0016 reg=2 y Ljava/lang/Object;
//      0x0016 - 0x0026 reg=0 ex Ljava/lang/Exception;
//      0x002d - 0x002f reg=3 y Ljava/lang/Object;
//      0x002f - 0x0030 reg=2 y Ljava/lang/Object;
//      0x0030 - 0x0032 reg=3 y Ljava/lang/Object;
//      0x0031 - 0x0033 reg=0 ex Ljava/lang/Exception;
//      0x0005 - 0x0033 reg=1 x [Ljava/lang/Object;
//      0x0032 - 0x0033 reg=2 y Ljava/lang/Object;
//      0x0000 - 0x0033 reg=8 this LMain;

extern "C" JNIEXPORT jint JNICALL Java_Main_refmap(JNIEnv*, jobject, jint count) {
  // Visitor
  ScopedObjectAccess soa(Thread::Current());
  ReferenceMap2Visitor mapper(soa.Self());
  mapper.WalkStack();

  return count + 1;
}

}  // namespace art
