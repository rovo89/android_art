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

#include "intern_table.h"

#include "common_runtime_test.h"
#include "mirror/object.h"
#include "handle_scope-inl.h"
#include "mirror/string.h"
#include "scoped_thread_state_change.h"

namespace art {

class InternTableTest : public CommonRuntimeTest {};

TEST_F(InternTableTest, Intern) {
  ScopedObjectAccess soa(Thread::Current());
  InternTable intern_table;
  StackHandleScope<4> hs(soa.Self());
  Handle<mirror::String> foo_1(hs.NewHandle(intern_table.InternStrong(3, "foo")));
  Handle<mirror::String> foo_2(hs.NewHandle(intern_table.InternStrong(3, "foo")));
  Handle<mirror::String> foo_3(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "foo")));
  Handle<mirror::String> bar(hs.NewHandle(intern_table.InternStrong(3, "bar")));
  EXPECT_TRUE(foo_1->Equals("foo"));
  EXPECT_TRUE(foo_2->Equals("foo"));
  EXPECT_TRUE(foo_3->Equals("foo"));
  EXPECT_TRUE(foo_1.Get() != NULL);
  EXPECT_TRUE(foo_2.Get() != NULL);
  EXPECT_EQ(foo_1.Get(), foo_2.Get());
  EXPECT_NE(foo_1.Get(), bar.Get());
  EXPECT_NE(foo_2.Get(), bar.Get());
  EXPECT_NE(foo_3.Get(), bar.Get());
}

TEST_F(InternTableTest, Size) {
  ScopedObjectAccess soa(Thread::Current());
  InternTable t;
  EXPECT_EQ(0U, t.Size());
  t.InternStrong(3, "foo");
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::String> foo(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "foo")));
  t.InternWeak(foo.Get());
  EXPECT_EQ(1U, t.Size());
  t.InternStrong(3, "bar");
  EXPECT_EQ(2U, t.Size());
}

class TestPredicate {
 public:
  bool IsMarked(const mirror::Object* s) const {
    bool erased = false;
    for (auto it = expected_.begin(), end = expected_.end(); it != end; ++it) {
      if (*it == s) {
        expected_.erase(it);
        erased = true;
        break;
      }
    }
    EXPECT_TRUE(erased);
    return false;
  }

  void Expect(const mirror::String* s) {
    expected_.push_back(s);
  }

  ~TestPredicate() {
    EXPECT_EQ(0U, expected_.size());
  }

 private:
  mutable std::vector<const mirror::String*> expected_;
};

mirror::Object* IsMarkedSweepingCallback(mirror::Object* object, void* arg) {
  if (reinterpret_cast<TestPredicate*>(arg)->IsMarked(object)) {
    return object;
  }
  return nullptr;
}

TEST_F(InternTableTest, SweepInternTableWeaks) {
  ScopedObjectAccess soa(Thread::Current());
  InternTable t;
  t.InternStrong(3, "foo");
  t.InternStrong(3, "bar");
  StackHandleScope<5> hs(soa.Self());
  Handle<mirror::String> hello(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello")));
  Handle<mirror::String> world(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "world")));
  Handle<mirror::String> s0(hs.NewHandle(t.InternWeak(hello.Get())));
  Handle<mirror::String> s1(hs.NewHandle(t.InternWeak(world.Get())));

  EXPECT_EQ(4U, t.Size());

  // We should traverse only the weaks...
  TestPredicate p;
  p.Expect(s0.Get());
  p.Expect(s1.Get());
  {
    ReaderMutexLock mu(soa.Self(), *Locks::heap_bitmap_lock_);
    t.SweepInternTableWeaks(IsMarkedSweepingCallback, &p);
  }

  EXPECT_EQ(2U, t.Size());

  // Just check that we didn't corrupt the map.
  Handle<mirror::String> still_here(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "still here")));
  t.InternWeak(still_here.Get());
  EXPECT_EQ(3U, t.Size());
}

TEST_F(InternTableTest, ContainsWeak) {
  ScopedObjectAccess soa(Thread::Current());
  {
    // Strongs are never weak.
    InternTable t;
    StackHandleScope<2> hs(soa.Self());
    Handle<mirror::String> interned_foo_1(hs.NewHandle(t.InternStrong(3, "foo")));
    EXPECT_FALSE(t.ContainsWeak(interned_foo_1.Get()));
    Handle<mirror::String> interned_foo_2(hs.NewHandle(t.InternStrong(3, "foo")));
    EXPECT_FALSE(t.ContainsWeak(interned_foo_2.Get()));
    EXPECT_EQ(interned_foo_1.Get(), interned_foo_2.Get());
  }

  {
    // Weaks are always weak.
    InternTable t;
    StackHandleScope<4> hs(soa.Self());
    Handle<mirror::String> foo_1(
        hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "foo")));
    Handle<mirror::String> foo_2(
        hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "foo")));
    EXPECT_NE(foo_1.Get(), foo_2.Get());
    Handle<mirror::String> interned_foo_1(hs.NewHandle(t.InternWeak(foo_1.Get())));
    Handle<mirror::String> interned_foo_2(hs.NewHandle(t.InternWeak(foo_2.Get())));
    EXPECT_TRUE(t.ContainsWeak(interned_foo_2.Get()));
    EXPECT_EQ(interned_foo_1.Get(), interned_foo_2.Get());
  }

  {
    // A weak can be promoted to a strong.
    InternTable t;
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::String> foo(
        hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "foo")));
    Handle<mirror::String> interned_foo_1(hs.NewHandle(t.InternWeak(foo.Get())));
    EXPECT_TRUE(t.ContainsWeak(interned_foo_1.Get()));
    Handle<mirror::String> interned_foo_2(hs.NewHandle(t.InternStrong(3, "foo")));
    EXPECT_FALSE(t.ContainsWeak(interned_foo_2.Get()));
    EXPECT_EQ(interned_foo_1.Get(), interned_foo_2.Get());
  }

  {
    // Interning a weak after a strong gets you the strong.
    InternTable t;
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::String> interned_foo_1(hs.NewHandle(t.InternStrong(3, "foo")));
    EXPECT_FALSE(t.ContainsWeak(interned_foo_1.Get()));
    Handle<mirror::String> foo(
        hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "foo")));
    Handle<mirror::String> interned_foo_2(hs.NewHandle(t.InternWeak(foo.Get())));
    EXPECT_FALSE(t.ContainsWeak(interned_foo_2.Get()));
    EXPECT_EQ(interned_foo_1.Get(), interned_foo_2.Get());
  }
}

}  // namespace art
