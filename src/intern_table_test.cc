// Copyright 2011 Google Inc. All Rights Reserved.

#include "intern_table.h"

#include "common_test.h"
#include "object.h"

namespace art {

class InternTableTest : public CommonTest {};

TEST_F(InternTableTest, Intern) {
  InternTable intern_table;
  SirtRef<String> foo_1(intern_table.InternStrong(3, "foo"));
  SirtRef<String> foo_2(intern_table.InternStrong(3, "foo"));
  SirtRef<String> foo_3(String::AllocFromModifiedUtf8("foo"));
  SirtRef<String> bar(intern_table.InternStrong(3, "bar"));
  EXPECT_TRUE(foo_1->Equals("foo"));
  EXPECT_TRUE(foo_2->Equals("foo"));
  EXPECT_TRUE(foo_3->Equals("foo"));
  EXPECT_TRUE(foo_1.get() != NULL);
  EXPECT_TRUE(foo_2.get() != NULL);
  EXPECT_EQ(foo_1.get(), foo_2.get());
  EXPECT_NE(foo_1.get(), bar.get());
  EXPECT_NE(foo_2.get(), bar.get());
  EXPECT_NE(foo_3.get(), bar.get());
}

TEST_F(InternTableTest, Size) {
  InternTable t;
  EXPECT_EQ(0U, t.Size());
  t.InternStrong(3, "foo");
  SirtRef<String> foo(String::AllocFromModifiedUtf8("foo"));
  t.InternWeak(foo.get());
  EXPECT_EQ(1U, t.Size());
  t.InternStrong(3, "bar");
  EXPECT_EQ(2U, t.Size());
}

class TestPredicate {
 public:
  bool IsMarked(const Object* s) const {
    bool erased = false;
    typedef std::vector<const String*>::iterator It; // TODO: C++0x auto
    for (It it = expected_.begin(), end = expected_.end(); it != end; ++it) {
      if (*it == s) {
        expected_.erase(it);
        erased = true;
        break;
      }
    }
    EXPECT_TRUE(erased);
    return false;
  }

  void Expect(const String* s) {
    expected_.push_back(s);
  }

  ~TestPredicate() {
    EXPECT_EQ(0U, expected_.size());
  }

 private:
  mutable std::vector<const String*> expected_;
};

bool IsMarked(const Object* object, void* arg) {
  return reinterpret_cast<TestPredicate*>(arg)->IsMarked(object);
}

TEST_F(InternTableTest, SweepInternTableWeaks) {
  InternTable t;
  t.InternStrong(3, "foo");
  t.InternStrong(3, "bar");
  SirtRef<String> hello(String::AllocFromModifiedUtf8("hello"));
  SirtRef<String> world(String::AllocFromModifiedUtf8("world"));
  SirtRef<String> s0(t.InternWeak(hello.get()));
  SirtRef<String> s1(t.InternWeak(world.get()));

  EXPECT_EQ(4U, t.Size());

  // We should traverse only the weaks...
  TestPredicate p;
  p.Expect(s0.get());
  p.Expect(s1.get());
  t.SweepInternTableWeaks(IsMarked, &p);

  EXPECT_EQ(2U, t.Size());

  // Just check that we didn't corrupt the map.
  SirtRef<String> still_here(String::AllocFromModifiedUtf8("still here"));
  t.InternWeak(still_here.get());
  EXPECT_EQ(3U, t.Size());
}

TEST_F(InternTableTest, ContainsWeak) {
  {
    // Strongs are never weak.
    InternTable t;
    SirtRef<String> interned_foo_1(t.InternStrong(3, "foo"));
    EXPECT_FALSE(t.ContainsWeak(interned_foo_1.get()));
    SirtRef<String> interned_foo_2(t.InternStrong(3, "foo"));
    EXPECT_FALSE(t.ContainsWeak(interned_foo_2.get()));
    EXPECT_EQ(interned_foo_1.get(), interned_foo_2.get());
  }

  {
    // Weaks are always weak.
    InternTable t;
    SirtRef<String> foo_1(String::AllocFromModifiedUtf8("foo"));
    SirtRef<String> foo_2(String::AllocFromModifiedUtf8("foo"));
    EXPECT_NE(foo_1.get(), foo_2.get());
    SirtRef<String> interned_foo_1(t.InternWeak(foo_1.get()));
    SirtRef<String> interned_foo_2(t.InternWeak(foo_2.get()));
    EXPECT_TRUE(t.ContainsWeak(interned_foo_2.get()));
    EXPECT_EQ(interned_foo_1.get(), interned_foo_2.get());
  }

  {
    // A weak can be promoted to a strong.
    InternTable t;
    SirtRef<String> foo(String::AllocFromModifiedUtf8("foo"));
    SirtRef<String> interned_foo_1(t.InternWeak(foo.get()));
    EXPECT_TRUE(t.ContainsWeak(interned_foo_1.get()));
    SirtRef<String> interned_foo_2(t.InternStrong(3, "foo"));
    EXPECT_FALSE(t.ContainsWeak(interned_foo_2.get()));
    EXPECT_EQ(interned_foo_1.get(), interned_foo_2.get());
  }

  {
    // Interning a weak after a strong gets you the strong.
    InternTable t;
    SirtRef<String> interned_foo_1(t.InternStrong(3, "foo"));
    EXPECT_FALSE(t.ContainsWeak(interned_foo_1.get()));
    SirtRef<String> foo(String::AllocFromModifiedUtf8("foo"));
    SirtRef<String> interned_foo_2(t.InternWeak(foo.get()));
    EXPECT_FALSE(t.ContainsWeak(interned_foo_2.get()));
    EXPECT_EQ(interned_foo_1.get(), interned_foo_2.get());
  }
}

}  // namespace art
