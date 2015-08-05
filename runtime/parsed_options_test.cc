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

#include "parsed_options.h"

#include <memory>

#include "common_runtime_test.h"

namespace art {

class ParsedOptionsTest : public ::testing::Test {
 public:
  static void SetUpTestCase() {
    CommonRuntimeTest::SetUpAndroidRoot();
  }
};

TEST_F(ParsedOptionsTest, ParsedOptions) {
  void* test_vfprintf = reinterpret_cast<void*>(0xa);
  void* test_abort = reinterpret_cast<void*>(0xb);
  void* test_exit = reinterpret_cast<void*>(0xc);

  std::string lib_core(CommonRuntimeTest::GetLibCoreDexFileName());

  std::string boot_class_path;
  boot_class_path += "-Xbootclasspath:";
  boot_class_path += lib_core;

  RuntimeOptions options;
  options.push_back(std::make_pair(boot_class_path.c_str(), nullptr));
  options.push_back(std::make_pair("-classpath", nullptr));
  options.push_back(std::make_pair(lib_core.c_str(), nullptr));
  options.push_back(std::make_pair("-cp", nullptr));
  options.push_back(std::make_pair(lib_core.c_str(), nullptr));
  options.push_back(std::make_pair("-Ximage:boot_image", nullptr));
  options.push_back(std::make_pair("-Xcheck:jni", nullptr));
  options.push_back(std::make_pair("-Xms2048", nullptr));
  options.push_back(std::make_pair("-Xmx4k", nullptr));
  options.push_back(std::make_pair("-Xss1m", nullptr));
  options.push_back(std::make_pair("-XX:HeapTargetUtilization=0.75", nullptr));
  options.push_back(std::make_pair("-Dfoo=bar", nullptr));
  options.push_back(std::make_pair("-Dbaz=qux", nullptr));
  options.push_back(std::make_pair("-verbose:gc,class,jni", nullptr));
  options.push_back(std::make_pair("vfprintf", test_vfprintf));
  options.push_back(std::make_pair("abort", test_abort));
  options.push_back(std::make_pair("exit", test_exit));

  RuntimeArgumentMap map;
  std::unique_ptr<ParsedOptions> parsed(ParsedOptions::Create(options, false, &map));
  ASSERT_TRUE(parsed.get() != nullptr);
  ASSERT_NE(0u, map.Size());

  using Opt = RuntimeArgumentMap;

#define EXPECT_PARSED_EQ(expected, actual_key) EXPECT_EQ(expected, map.GetOrDefault(actual_key))
#define EXPECT_PARSED_EXISTS(actual_key) EXPECT_TRUE(map.Exists(actual_key))

  EXPECT_PARSED_EQ(lib_core, Opt::BootClassPath);
  EXPECT_PARSED_EQ(lib_core, Opt::ClassPath);
  EXPECT_PARSED_EQ(std::string("boot_image"), Opt::Image);
  EXPECT_PARSED_EXISTS(Opt::CheckJni);
  EXPECT_PARSED_EQ(2048U, Opt::MemoryInitialSize);
  EXPECT_PARSED_EQ(4 * KB, Opt::MemoryMaximumSize);
  EXPECT_PARSED_EQ(1 * MB, Opt::StackSize);
  EXPECT_DOUBLE_EQ(0.75, map.GetOrDefault(Opt::HeapTargetUtilization));
  EXPECT_TRUE(test_vfprintf == map.GetOrDefault(Opt::HookVfprintf));
  EXPECT_TRUE(test_exit == map.GetOrDefault(Opt::HookExit));
  EXPECT_TRUE(test_abort == map.GetOrDefault(Opt::HookAbort));
  EXPECT_TRUE(VLOG_IS_ON(class_linker));
  EXPECT_FALSE(VLOG_IS_ON(compiler));
  EXPECT_FALSE(VLOG_IS_ON(heap));
  EXPECT_TRUE(VLOG_IS_ON(gc));
  EXPECT_FALSE(VLOG_IS_ON(jdwp));
  EXPECT_TRUE(VLOG_IS_ON(jni));
  EXPECT_FALSE(VLOG_IS_ON(monitor));
  EXPECT_FALSE(VLOG_IS_ON(startup));
  EXPECT_FALSE(VLOG_IS_ON(third_party_jni));
  EXPECT_FALSE(VLOG_IS_ON(threads));

  auto&& properties_list = map.GetOrDefault(Opt::PropertiesList);
  ASSERT_EQ(2U, properties_list.size());
  EXPECT_EQ("foo=bar", properties_list[0]);
  EXPECT_EQ("baz=qux", properties_list[1]);
}

TEST_F(ParsedOptionsTest, ParsedOptionsGc) {
  RuntimeOptions options;
  options.push_back(std::make_pair("-Xgc:MC", nullptr));

  RuntimeArgumentMap map;
  std::unique_ptr<ParsedOptions> parsed(ParsedOptions::Create(options, false, &map));
  ASSERT_TRUE(parsed.get() != nullptr);
  ASSERT_NE(0u, map.Size());

  using Opt = RuntimeArgumentMap;

  EXPECT_TRUE(map.Exists(Opt::GcOption));

  XGcOption xgc = map.GetOrDefault(Opt::GcOption);
  EXPECT_EQ(gc::kCollectorTypeMC, xgc.collector_type_);}

}  // namespace art
