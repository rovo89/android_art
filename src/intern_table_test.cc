// Copyright 2011 Google Inc. All Rights Reserved.

#include "intern_table.h"

#include "common_test.h"
#include "object.h"

namespace art {

class InternTableTest : public CommonTest {};

TEST_F(InternTableTest, Intern) {
  InternTable intern_table;
  const String* foo_1 = intern_table.InternStrong(3, "foo");
  const String* foo_2 = intern_table.InternStrong(3, "foo");
  const String* foo_3 = String::AllocFromModifiedUtf8("foo");
  const String* bar = intern_table.InternStrong(3, "bar");
  EXPECT_TRUE(foo_1->Equals("foo"));
  EXPECT_TRUE(foo_2->Equals("foo"));
  EXPECT_TRUE(foo_3->Equals("foo"));
  EXPECT_TRUE(foo_1 != NULL);
  EXPECT_TRUE(foo_2 != NULL);
  EXPECT_EQ(foo_1, foo_2);
  EXPECT_NE(foo_1, bar);
  EXPECT_NE(foo_2, bar);
  EXPECT_NE(foo_3, bar);
}

TEST_F(InternTableTest, Size) {
  InternTable t;
  EXPECT_EQ(0U, t.Size());
  t.InternStrong(3, "foo");
  t.InternWeak(String::AllocFromModifiedUtf8("foo"));
  EXPECT_EQ(1U, t.Size());
  t.InternStrong(3, "bar");
  EXPECT_EQ(2U, t.Size());
}

class TestPredicate : public InternTable::Predicate {
 public:
  bool operator()(const String* s) const {
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
    return true;
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

TEST_F(InternTableTest, RemoveWeakIf) {
  InternTable t;
  t.InternStrong(3, "foo");
  t.InternStrong(3, "bar");
  const String* s0 = t.InternWeak(String::AllocFromModifiedUtf8("hello"));
  const String* s1 = t.InternWeak(String::AllocFromModifiedUtf8("world"));

  EXPECT_EQ(4U, t.Size());

  // We should traverse only the weaks...
  TestPredicate p;
  p.Expect(s0);
  p.Expect(s1);
  t.RemoveWeakIf(p);

  EXPECT_EQ(2U, t.Size());

  // Just check that we didn't corrupt the unordered_multimap.
  t.InternWeak(String::AllocFromModifiedUtf8("still here"));
  EXPECT_EQ(3U, t.Size());
}

TEST_F(InternTableTest, ContainsWeak) {
  {
    // Strongs are never weak.
    InternTable t;
    const String* foo_1 = t.InternStrong(3, "foo");
    EXPECT_FALSE(t.ContainsWeak(foo_1));
    const String* foo_2 = t.InternStrong(3, "foo");
    EXPECT_FALSE(t.ContainsWeak(foo_2));
    EXPECT_EQ(foo_1, foo_2);
  }

  {
    // Weaks are always weak.
    InternTable t;
    const String* foo_1 = t.InternWeak(String::AllocFromModifiedUtf8("foo"));
    EXPECT_TRUE(t.ContainsWeak(foo_1));
    const String* foo_2 = t.InternWeak(String::AllocFromModifiedUtf8("foo"));
    EXPECT_TRUE(t.ContainsWeak(foo_2));
    EXPECT_EQ(foo_1, foo_2);
  }

  {
    // A weak can be promoted to a strong.
    InternTable t;
    const String* foo_1 = t.InternWeak(String::AllocFromModifiedUtf8("foo"));
    EXPECT_TRUE(t.ContainsWeak(foo_1));
    const String* foo_2 = t.InternStrong(3, "foo");
    EXPECT_FALSE(t.ContainsWeak(foo_2));
    EXPECT_EQ(foo_1, foo_2);
  }

  {
    // Interning a weak after a strong gets you the strong.
    InternTable t;
    const String* foo_1 = t.InternStrong(3, "foo");
    EXPECT_FALSE(t.ContainsWeak(foo_1));
    const String* foo_2 = t.InternWeak(String::AllocFromModifiedUtf8("foo"));
    EXPECT_FALSE(t.ContainsWeak(foo_2));
    EXPECT_EQ(foo_1, foo_2);
  }
}

}  // namespace art
