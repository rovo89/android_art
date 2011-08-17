// Copyright 2011 Google Inc. All Rights Reserved.

#include "runtime.h"
#include "common_test.h"

namespace art {
void ParseClassPath(const char* class_path, std::vector<std::string>& vec);

class RuntimeTest : public CommonTest {};

TEST_F(RuntimeTest, ParseClassPath) {
  std::vector<std::string> vec;

  art::ParseClassPath("", vec);
  EXPECT_EQ(0U, vec.size());
  vec.clear();

  art::ParseClassPath(":", vec);
  EXPECT_EQ(0U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo", vec);
  EXPECT_EQ(1U, vec.size());
  vec.clear();

  art::ParseClassPath("foo:", vec);
  EXPECT_EQ(1U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo:", vec);
  EXPECT_EQ(1U, vec.size());
  vec.clear();

  art::ParseClassPath("foo:bar", vec);
  EXPECT_EQ(2U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo:bar", vec);
  EXPECT_EQ(2U, vec.size());
  vec.clear();

  art::ParseClassPath("foo:bar:", vec);
  EXPECT_EQ(2U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo:bar:", vec);
  EXPECT_EQ(2U, vec.size());
  vec.clear();

  art::ParseClassPath("foo:bar:baz", vec);
  EXPECT_EQ(3U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo:bar:baz", vec);
  EXPECT_EQ(3U, vec.size());
  vec.clear();

  art::ParseClassPath("foo:bar:baz:", vec);
  EXPECT_EQ(3U, vec.size());
  vec.clear();

  art::ParseClassPath(":foo:bar:baz:", vec);
  EXPECT_EQ(3U, vec.size());
  vec.clear();
}

TEST_F(RuntimeTest, ParsedOptions) {
  void* test_vfprintf = reinterpret_cast<void*>(0xa);
  void* test_abort = reinterpret_cast<void*>(0xb);
  void* test_exit = reinterpret_cast<void*>(0xc);
  void* null = reinterpret_cast<void*>(NULL);
  scoped_ptr<const DexFile> java_lang_dex_file(GetLibCoreDex());
  std::vector<const DexFile*> boot_class_path;
  boot_class_path.push_back(java_lang_dex_file_.get());

  Runtime::Options options;
  options.push_back(std::make_pair("-Xbootclasspath:class_path_foo:class_path_bar", null));
  options.push_back(std::make_pair("bootclasspath", &boot_class_path));
  options.push_back(std::make_pair("-Xbootimage:boot_image", null));
  options.push_back(std::make_pair("-Xcheck:jni", null));
  options.push_back(std::make_pair("-Xms2048", null));
  options.push_back(std::make_pair("-Xmx4k", null));
  options.push_back(std::make_pair("-Xss1m", null));
  options.push_back(std::make_pair("-Dfoo=bar", null));
  options.push_back(std::make_pair("-Dbaz=qux", null));
  options.push_back(std::make_pair("-verbose:gc,class,jni", null));
  options.push_back(std::make_pair("vfprintf", test_vfprintf));
  options.push_back(std::make_pair("abort", test_abort));
  options.push_back(std::make_pair("exit", test_exit));
  scoped_ptr<Runtime::ParsedOptions> parsed(Runtime::ParsedOptions::Create(options, false));
  ASSERT_TRUE(parsed != NULL);

  EXPECT_EQ(1U, parsed->boot_class_path_.size());  // bootclasspath overrides -Xbootclasspath
  EXPECT_STREQ("boot_image", parsed->boot_image_);
  EXPECT_EQ(true, parsed->check_jni_);
  EXPECT_EQ(2048U, parsed->heap_initial_size_);
  EXPECT_EQ(4 * KB, parsed->heap_maximum_size_);
  EXPECT_EQ(1 * MB, parsed->stack_size_);
  EXPECT_TRUE(test_vfprintf == parsed->hook_vfprintf_);
  EXPECT_TRUE(test_exit == parsed->hook_exit_);
  EXPECT_TRUE(test_abort == parsed->hook_abort_);
  ASSERT_EQ(3U, parsed->verbose_.size());
  EXPECT_TRUE(parsed->verbose_.find("gc") != parsed->verbose_.end());
  EXPECT_TRUE(parsed->verbose_.find("class") != parsed->verbose_.end());
  EXPECT_TRUE(parsed->verbose_.find("jni") != parsed->verbose_.end());
  ASSERT_EQ(2U, parsed->properties_.size());
  EXPECT_EQ("foo=bar", parsed->properties_[0]);
  EXPECT_EQ("baz=qux", parsed->properties_[1]);
}

}  // namespace art
