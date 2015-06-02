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

#include "arch/context.h"
#include "art_method-inl.h"
#include "jni.h"
#include "scoped_thread_state_change.h"
#include "stack.h"
#include "thread.h"

namespace art {

namespace {

class TestVisitor : public StackVisitor {
 public:
  TestVisitor(Thread* thread, Context* context, mirror::Object* this_value)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        this_value_(this_value) {}

  bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ArtMethod* m = GetMethod();
    std::string m_name(m->GetName());

    if (m_name.compare("testIntVReg") == 0) {
      uint32_t value = 0;
      CHECK(GetVReg(m, 1, kReferenceVReg, &value));
      CHECK_EQ(reinterpret_cast<mirror::Object*>(value), this_value_);

      CHECK(SetVReg(m, 2, 5, kIntVReg));
      CHECK(SetVReg(m, 3, 4, kIntVReg));
      CHECK(SetVReg(m, 4, 3, kIntVReg));
      CHECK(SetVReg(m, 5, 2, kIntVReg));
      CHECK(SetVReg(m, 6, 1, kIntVReg));
    } else if (m_name.compare("testLongVReg") == 0) {
      uint32_t value = 0;
      CHECK(GetVReg(m, 3, kReferenceVReg, &value));
      CHECK_EQ(reinterpret_cast<mirror::Object*>(value), this_value_);

      CHECK(SetVRegPair(m, 4, std::numeric_limits<int64_t>::max(), kLongLoVReg, kLongHiVReg));
      CHECK(SetVRegPair(m, 6, 4, kLongLoVReg, kLongHiVReg));
      CHECK(SetVRegPair(m, 8, 3, kLongLoVReg, kLongHiVReg));
      CHECK(SetVRegPair(m, 10, 2, kLongLoVReg, kLongHiVReg));
      CHECK(SetVRegPair(m, 12, 1, kLongLoVReg, kLongHiVReg));
    } else if (m_name.compare("testFloatVReg") == 0) {
      uint32_t value = 0;
      CHECK(GetVReg(m, 1, kReferenceVReg, &value));
      CHECK_EQ(reinterpret_cast<mirror::Object*>(value), this_value_);

      CHECK(SetVReg(m, 2, bit_cast<uint32_t, float>(5.0f), kFloatVReg));
      CHECK(SetVReg(m, 3, bit_cast<uint32_t, float>(4.0f), kFloatVReg));
      CHECK(SetVReg(m, 4, bit_cast<uint32_t, float>(3.0f), kFloatVReg));
      CHECK(SetVReg(m, 5, bit_cast<uint32_t, float>(2.0f), kFloatVReg));
      CHECK(SetVReg(m, 6, bit_cast<uint32_t, float>(1.0f), kFloatVReg));
    } else if (m_name.compare("testDoubleVReg") == 0) {
      uint32_t value = 0;
      CHECK(GetVReg(m, 3, kReferenceVReg, &value));
      CHECK_EQ(reinterpret_cast<mirror::Object*>(value), this_value_);

      CHECK(SetVRegPair(m, 4, bit_cast<uint64_t, double>(5.0), kDoubleLoVReg, kDoubleHiVReg));
      CHECK(SetVRegPair(m, 6, bit_cast<uint64_t, double>(4.0), kDoubleLoVReg, kDoubleHiVReg));
      CHECK(SetVRegPair(m, 8, bit_cast<uint64_t, double>(3.0), kDoubleLoVReg, kDoubleHiVReg));
      CHECK(SetVRegPair(m, 10, bit_cast<uint64_t, double>(2.0), kDoubleLoVReg, kDoubleHiVReg));
      CHECK(SetVRegPair(m, 12, bit_cast<uint64_t, double>(1.0), kDoubleLoVReg, kDoubleHiVReg));
    }

    return true;
  }

  mirror::Object* this_value_;
};

extern "C" JNIEXPORT void JNICALL Java_Main_doNativeCallSetVReg(JNIEnv*, jobject value) {
  ScopedObjectAccess soa(Thread::Current());
  std::unique_ptr<Context> context(Context::Create());
  TestVisitor visitor(soa.Self(), context.get(), soa.Decode<mirror::Object*>(value));
  visitor.WalkStack();
}

}  // namespace

}  // namespace art
