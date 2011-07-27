// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/runtime.h"

#include "gtest/gtest.h"

namespace art {
void ParseClassPath(const char* class_path, std::vector<std::string>* vec);
}

namespace {

TEST(RuntimeTest, ParseClassPath) {
  std::vector<std::string> vec;

  art::ParseClassPath("", &vec);
  EXPECT_EQ(0U, vec.size());
  vec.clear();

  art::ParseClassPath(":", &vec);
  EXPECT_EQ(0U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo", &vec);
  EXPECT_EQ(1U, vec.size());
  vec.clear();

  art::ParseClassPath("foo:", &vec);
  EXPECT_EQ(1U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo:", &vec);
  EXPECT_EQ(1U, vec.size());
  vec.clear();

  art::ParseClassPath("foo:bar", &vec);
  EXPECT_EQ(2U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo:bar", &vec);
  EXPECT_EQ(2U, vec.size());
  vec.clear();

  art::ParseClassPath("foo:bar:", &vec);
  EXPECT_EQ(2U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo:bar:", &vec);
  EXPECT_EQ(2U, vec.size());
  vec.clear();

  art::ParseClassPath("foo:bar:baz", &vec);
  EXPECT_EQ(3U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo:bar:baz", &vec);
  EXPECT_EQ(3U, vec.size());
  vec.clear();

  art::ParseClassPath("foo:bar:baz:", &vec);
  EXPECT_EQ(3U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo:bar:baz:", &vec);
  EXPECT_EQ(3U, vec.size());
  vec.clear();
}

}  // namespace
