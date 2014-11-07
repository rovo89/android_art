/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "hash_set.h"

#include <map>
#include <sstream>
#include <string>
#include <unordered_set>

#include "common_runtime_test.h"
#include "hash_map.h"

namespace art {

struct IsEmptyFnString {
  void MakeEmpty(std::string& item) const {
    item.clear();
  }
  bool IsEmpty(const std::string& item) const {
    return item.empty();
  }
};

class HashSetTest : public CommonRuntimeTest {
 public:
  HashSetTest() : seed_(97421), unique_number_(0) {
  }
  std::string RandomString(size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
      oss << static_cast<char>('A' + PRand() % 64);
    }
    static_assert(' ' < 'A', "space must be less than a");
    oss << " " << unique_number_++;  // Relies on ' ' < 'A'
    return oss.str();
  }
  void SetSeed(size_t seed) {
    seed_ = seed;
  }
  size_t PRand() {  // Pseudo random.
    seed_ = seed_ * 1103515245 + 12345;
    return seed_;
  }

 private:
  size_t seed_;
  size_t unique_number_;
};

TEST_F(HashSetTest, TestSmoke) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  const std::string test_string = "hello world 1234";
  ASSERT_TRUE(hash_set.Empty());
  ASSERT_EQ(hash_set.Size(), 0U);
  hash_set.Insert(test_string);
  auto it = hash_set.Find(test_string);
  ASSERT_EQ(*it, test_string);
  auto after_it = hash_set.Erase(it);
  ASSERT_TRUE(after_it == hash_set.end());
  ASSERT_TRUE(hash_set.Empty());
  ASSERT_EQ(hash_set.Size(), 0U);
  it = hash_set.Find(test_string);
  ASSERT_TRUE(it == hash_set.end());
}

TEST_F(HashSetTest, TestInsertAndErase) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  static constexpr size_t count = 1000;
  std::vector<std::string> strings;
  for (size_t i = 0; i < count; ++i) {
    // Insert a bunch of elements and make sure we can find them.
    strings.push_back(RandomString(10));
    hash_set.Insert(strings[i]);
    auto it = hash_set.Find(strings[i]);
    ASSERT_TRUE(it != hash_set.end());
    ASSERT_EQ(*it, strings[i]);
  }
  ASSERT_EQ(strings.size(), hash_set.Size());
  // Try to erase the odd strings.
  for (size_t i = 1; i < count; i += 2) {
    auto it = hash_set.Find(strings[i]);
    ASSERT_TRUE(it != hash_set.end());
    ASSERT_EQ(*it, strings[i]);
    hash_set.Erase(it);
  }
  // Test removed.
  for (size_t i = 1; i < count; i += 2) {
    auto it = hash_set.Find(strings[i]);
    ASSERT_TRUE(it == hash_set.end());
  }
  for (size_t i = 0; i < count; i += 2) {
    auto it = hash_set.Find(strings[i]);
    ASSERT_TRUE(it != hash_set.end());
    ASSERT_EQ(*it, strings[i]);
  }
}

TEST_F(HashSetTest, TestIterator) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  ASSERT_TRUE(hash_set.begin() == hash_set.end());
  static constexpr size_t count = 1000;
  std::vector<std::string> strings;
  for (size_t i = 0; i < count; ++i) {
    // Insert a bunch of elements and make sure we can find them.
    strings.push_back(RandomString(10));
    hash_set.Insert(strings[i]);
  }
  // Make sure we visit each string exactly once.
  std::map<std::string, size_t> found_count;
  for (const std::string& s : hash_set) {
    ++found_count[s];
  }
  for (size_t i = 0; i < count; ++i) {
    ASSERT_EQ(found_count[strings[i]], 1U);
  }
  found_count.clear();
  // Remove all the elements with iterator erase.
  for (auto it = hash_set.begin(); it != hash_set.end();) {
    ++found_count[*it];
    it = hash_set.Erase(it);
    ASSERT_EQ(hash_set.Verify(), 0U);
  }
  for (size_t i = 0; i < count; ++i) {
    ASSERT_EQ(found_count[strings[i]], 1U);
  }
}

TEST_F(HashSetTest, TestSwap) {
  HashSet<std::string, IsEmptyFnString> hash_seta, hash_setb;
  std::vector<std::string> strings;
  static constexpr size_t count = 1000;
  for (size_t i = 0; i < count; ++i) {
    strings.push_back(RandomString(10));
    hash_seta.Insert(strings[i]);
  }
  std::swap(hash_seta, hash_setb);
  hash_seta.Insert("TEST");
  hash_setb.Insert("TEST2");
  for (size_t i = 0; i < count; ++i) {
    strings.push_back(RandomString(10));
    hash_seta.Insert(strings[i]);
  }
}

TEST_F(HashSetTest, TestStress) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  std::unordered_multiset<std::string> std_set;
  std::vector<std::string> strings;
  static constexpr size_t string_count = 2000;
  static constexpr size_t operations = 100000;
  static constexpr size_t target_size = 5000;
  for (size_t i = 0; i < string_count; ++i) {
    strings.push_back(RandomString(i % 10 + 1));
  }
  const size_t seed = time(nullptr);
  SetSeed(seed);
  LOG(INFO) << "Starting stress test with seed " << seed;
  for (size_t i = 0; i < operations; ++i) {
    ASSERT_EQ(hash_set.Size(), std_set.size());
    size_t delta = std::abs(static_cast<ssize_t>(target_size) -
                            static_cast<ssize_t>(hash_set.Size()));
    size_t n = PRand();
    if (n % target_size == 0) {
      hash_set.Clear();
      std_set.clear();
      ASSERT_TRUE(hash_set.Empty());
      ASSERT_TRUE(std_set.empty());
    } else  if (n % target_size < delta) {
      // Skew towards adding elements until we are at the desired size.
      const std::string& s = strings[PRand() % string_count];
      hash_set.Insert(s);
      std_set.insert(s);
      ASSERT_EQ(*hash_set.Find(s), *std_set.find(s));
    } else {
      const std::string& s = strings[PRand() % string_count];
      auto it1 = hash_set.Find(s);
      auto it2 = std_set.find(s);
      ASSERT_EQ(it1 == hash_set.end(), it2 == std_set.end());
      if (it1 != hash_set.end()) {
        ASSERT_EQ(*it1, *it2);
        hash_set.Erase(it1);
        std_set.erase(it2);
      }
    }
  }
}

struct IsEmptyStringPair {
  void MakeEmpty(std::pair<std::string, int>& pair) const {
    pair.first.clear();
  }
  bool IsEmpty(const std::pair<std::string, int>& pair) const {
    return pair.first.empty();
  }
};

TEST_F(HashSetTest, TestHashMap) {
  HashMap<std::string, int, IsEmptyStringPair> hash_map;
  hash_map.Insert(std::make_pair(std::string("abcd"), 123));
  hash_map.Insert(std::make_pair(std::string("abcd"), 124));
  hash_map.Insert(std::make_pair(std::string("bags"), 444));
  auto it = hash_map.Find(std::string("abcd"));
  ASSERT_EQ(it->second, 123);
  hash_map.Erase(it);
  it = hash_map.Find(std::string("abcd"));
  ASSERT_EQ(it->second, 124);
}

}  // namespace art
