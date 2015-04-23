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

#include "cmdline_parser.h"
#include "runtime/runtime_options.h"
#include "runtime/parsed_options.h"

#include "utils.h"
#include <numeric>
#include "gtest/gtest.h"

#define EXPECT_NULL(expected) EXPECT_EQ(reinterpret_cast<const void*>(expected), \
                                        reinterpret_cast<void*>(nullptr));

namespace art {
  bool UsuallyEquals(double expected, double actual);

  // This has a gtest dependency, which is why it's in the gtest only.
  bool operator==(const TestProfilerOptions& lhs, const TestProfilerOptions& rhs) {
    return lhs.enabled_ == rhs.enabled_ &&
        lhs.output_file_name_ == rhs.output_file_name_ &&
        lhs.period_s_ == rhs.period_s_ &&
        lhs.duration_s_ == rhs.duration_s_ &&
        lhs.interval_us_ == rhs.interval_us_ &&
        UsuallyEquals(lhs.backoff_coefficient_, rhs.backoff_coefficient_) &&
        UsuallyEquals(lhs.start_immediately_, rhs.start_immediately_) &&
        UsuallyEquals(lhs.top_k_threshold_, rhs.top_k_threshold_) &&
        UsuallyEquals(lhs.top_k_change_threshold_, rhs.top_k_change_threshold_) &&
        lhs.profile_type_ == rhs.profile_type_ &&
        lhs.max_stack_depth_ == rhs.max_stack_depth_;
  }

  bool UsuallyEquals(double expected, double actual) {
    using FloatingPoint = ::testing::internal::FloatingPoint<double>;

    FloatingPoint exp(expected);
    FloatingPoint act(actual);

    // Compare with ULPs instead of comparing with ==
    return exp.AlmostEquals(act);
  }

  template <typename T>
  bool UsuallyEquals(const T& expected, const T& actual,
                     typename std::enable_if<
                         detail::SupportsEqualityOperator<T>::value>::type* = 0) {
    return expected == actual;
  }

  // Try to use memcmp to compare simple plain-old-data structs.
  //
  // This should *not* generate false positives, but it can generate false negatives.
  // This will mostly work except for fields like float which can have different bit patterns
  // that are nevertheless equal.
  // If a test is failing because the structs aren't "equal" when they really are
  // then it's recommended to implement operator== for it instead.
  template <typename T, typename ... Ignore>
  bool UsuallyEquals(const T& expected, const T& actual,
                     const Ignore& ... more ATTRIBUTE_UNUSED,
                     typename std::enable_if<std::is_pod<T>::value>::type* = 0,
                     typename std::enable_if<!detail::SupportsEqualityOperator<T>::value>::type* = 0
                     ) {
    return memcmp(std::addressof(expected), std::addressof(actual), sizeof(T)) == 0;
  }

  bool UsuallyEquals(const XGcOption& expected, const XGcOption& actual) {
    return memcmp(std::addressof(expected), std::addressof(actual), sizeof(expected)) == 0;
  }

  bool UsuallyEquals(const char* expected, std::string actual) {
    return std::string(expected) == actual;
  }

  template <typename TMap, typename TKey, typename T>
  ::testing::AssertionResult IsExpectedKeyValue(const T& expected,
                                                const TMap& map,
                                                const TKey& key) {
    auto* actual = map.Get(key);
    if (actual != nullptr) {
      if (!UsuallyEquals(expected, *actual)) {
        return ::testing::AssertionFailure()
          << "expected " << detail::ToStringAny(expected) << " but got "
          << detail::ToStringAny(*actual);
      }
      return ::testing::AssertionSuccess();
    }

    return ::testing::AssertionFailure() << "key was not in the map";
  }

class CmdlineParserTest : public ::testing::Test {
 public:
  CmdlineParserTest() = default;
  ~CmdlineParserTest() = default;

 protected:
  using M = RuntimeArgumentMap;
  using RuntimeParser = ParsedOptions::RuntimeParser;

  static void SetUpTestCase() {
    art::InitLogging(nullptr);  // argv = null
  }

  virtual void SetUp() {
    parser_ = ParsedOptions::MakeParser(false);  // do not ignore unrecognized options
  }

  static ::testing::AssertionResult IsResultSuccessful(CmdlineResult result) {
    if (result.IsSuccess()) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure()
        << result.GetStatus() << " with: " << result.GetMessage();
    }
  }

  static ::testing::AssertionResult IsResultFailure(CmdlineResult result,
                                                    CmdlineResult::Status failure_status) {
    if (result.IsSuccess()) {
      return ::testing::AssertionFailure() << " got success but expected failure: "
          << failure_status;
    } else if (result.GetStatus() == failure_status) {
      return ::testing::AssertionSuccess();
    }

    return ::testing::AssertionFailure() << " expected failure " << failure_status
        << " but got " << result.GetStatus();
  }

  std::unique_ptr<RuntimeParser> parser_;
};

#define EXPECT_KEY_EXISTS(map, key) EXPECT_TRUE((map).Exists(key))
#define EXPECT_KEY_VALUE(map, key, expected) EXPECT_TRUE(IsExpectedKeyValue(expected, map, key))

#define EXPECT_SINGLE_PARSE_EMPTY_SUCCESS(argv)               \
  do {                                                        \
    EXPECT_TRUE(IsResultSuccessful(parser_->Parse(argv)));    \
    EXPECT_EQ(0u, parser_->GetArgumentsMap().Size());         \
  } while (false)

#define _EXPECT_SINGLE_PARSE_EXISTS(argv, key)                \
  do {                                                        \
    EXPECT_TRUE(IsResultSuccessful(parser_->Parse(argv)));    \
    RuntimeArgumentMap args = parser_->ReleaseArgumentsMap(); \
    EXPECT_EQ(1u, args.Size());                               \
    EXPECT_KEY_EXISTS(args, key);                             \

#define EXPECT_SINGLE_PARSE_EXISTS(argv, key)                 \
    _EXPECT_SINGLE_PARSE_EXISTS(argv, key);                   \
  } while (false)

#define EXPECT_SINGLE_PARSE_VALUE(expected, argv, key)        \
    _EXPECT_SINGLE_PARSE_EXISTS(argv, key);                   \
    EXPECT_KEY_VALUE(args, key, expected);                    \
  } while (false)                                             // NOLINT [readability/namespace] [5]

#define EXPECT_SINGLE_PARSE_VALUE_STR(expected, argv, key)    \
  EXPECT_SINGLE_PARSE_VALUE(std::string(expected), argv, key)

#define EXPECT_SINGLE_PARSE_FAIL(argv, failure_status)         \
    do {                                                       \
      EXPECT_TRUE(IsResultFailure(parser_->Parse(argv), failure_status));\
      RuntimeArgumentMap args = parser_->ReleaseArgumentsMap();\
      EXPECT_EQ(0u, args.Size());                              \
    } while (false)

TEST_F(CmdlineParserTest, TestSimpleSuccesses) {
  auto& parser = *parser_;

  EXPECT_LT(0u, parser.CountDefinedArguments());

  {
    // Test case 1: No command line arguments
    EXPECT_TRUE(IsResultSuccessful(parser.Parse("")));
    RuntimeArgumentMap args = parser.ReleaseArgumentsMap();
    EXPECT_EQ(0u, args.Size());
  }

  EXPECT_SINGLE_PARSE_EXISTS("-Xzygote", M::Zygote);
  EXPECT_SINGLE_PARSE_VALUE_STR("/hello/world", "-Xbootclasspath:/hello/world", M::BootClassPath);
  EXPECT_SINGLE_PARSE_VALUE("/hello/world", "-Xbootclasspath:/hello/world", M::BootClassPath);
  EXPECT_SINGLE_PARSE_VALUE(false, "-Xverify:none", M::Verify);
  EXPECT_SINGLE_PARSE_VALUE(true, "-Xverify:remote", M::Verify);
  EXPECT_SINGLE_PARSE_VALUE(true, "-Xverify:all", M::Verify);
  EXPECT_SINGLE_PARSE_VALUE(Memory<1>(234), "-Xss234", M::StackSize);
  EXPECT_SINGLE_PARSE_VALUE(MemoryKiB(1234*MB), "-Xms1234m", M::MemoryInitialSize);
  EXPECT_SINGLE_PARSE_VALUE(true, "-XX:EnableHSpaceCompactForOOM", M::EnableHSpaceCompactForOOM);
  EXPECT_SINGLE_PARSE_VALUE(false, "-XX:DisableHSpaceCompactForOOM", M::EnableHSpaceCompactForOOM);
  EXPECT_SINGLE_PARSE_VALUE(0.5, "-XX:HeapTargetUtilization=0.5", M::HeapTargetUtilization);
  EXPECT_SINGLE_PARSE_VALUE(5u, "-XX:ParallelGCThreads=5", M::ParallelGCThreads);
  EXPECT_SINGLE_PARSE_EXISTS("-Xno-dex-file-fallback", M::NoDexFileFallback);
}  // TEST_F

TEST_F(CmdlineParserTest, TestSimpleFailures) {
  // Test argument is unknown to the parser
  EXPECT_SINGLE_PARSE_FAIL("abcdefg^%@#*(@#", CmdlineResult::kUnknown);
  // Test value map substitution fails
  EXPECT_SINGLE_PARSE_FAIL("-Xverify:whatever", CmdlineResult::kFailure);
  // Test value type parsing failures
  EXPECT_SINGLE_PARSE_FAIL("-Xsswhatever", CmdlineResult::kFailure);  // invalid memory value
  EXPECT_SINGLE_PARSE_FAIL("-Xms123", CmdlineResult::kFailure);       // memory value too small
  EXPECT_SINGLE_PARSE_FAIL("-XX:HeapTargetUtilization=0.0", CmdlineResult::kOutOfRange);  // toosmal
  EXPECT_SINGLE_PARSE_FAIL("-XX:HeapTargetUtilization=2.0", CmdlineResult::kOutOfRange);  // toolarg
  EXPECT_SINGLE_PARSE_FAIL("-XX:ParallelGCThreads=-5", CmdlineResult::kOutOfRange);  // too small
  EXPECT_SINGLE_PARSE_FAIL("-Xgc:blablabla", CmdlineResult::kUsage);  // not a valid suboption
}  // TEST_F

TEST_F(CmdlineParserTest, TestLogVerbosity) {
  {
    const char* log_args = "-verbose:"
        "class,compiler,gc,heap,jdwp,jni,monitor,profiler,signals,startup,third-party-jni,"
        "threads,verifier";

    LogVerbosity log_verbosity = LogVerbosity();
    log_verbosity.class_linker = true;
    log_verbosity.compiler = true;
    log_verbosity.gc = true;
    log_verbosity.heap = true;
    log_verbosity.jdwp = true;
    log_verbosity.jni = true;
    log_verbosity.monitor = true;
    log_verbosity.profiler = true;
    log_verbosity.signals = true;
    log_verbosity.startup = true;
    log_verbosity.third_party_jni = true;
    log_verbosity.threads = true;
    log_verbosity.verifier = true;

    EXPECT_SINGLE_PARSE_VALUE(log_verbosity, log_args, M::Verbose);
  }

  {
    const char* log_args = "-verbose:"
        "class,compiler,gc,heap,jdwp,jni,monitor";

    LogVerbosity log_verbosity = LogVerbosity();
    log_verbosity.class_linker = true;
    log_verbosity.compiler = true;
    log_verbosity.gc = true;
    log_verbosity.heap = true;
    log_verbosity.jdwp = true;
    log_verbosity.jni = true;
    log_verbosity.monitor = true;

    EXPECT_SINGLE_PARSE_VALUE(log_verbosity, log_args, M::Verbose);
  }

  EXPECT_SINGLE_PARSE_FAIL("-verbose:blablabla", CmdlineResult::kUsage);  // invalid verbose opt

  {
    const char* log_args = "-verbose:oat";
    LogVerbosity log_verbosity = LogVerbosity();
    log_verbosity.oat = true;
    EXPECT_SINGLE_PARSE_VALUE(log_verbosity, log_args, M::Verbose);
  }
}  // TEST_F

// TODO: Enable this b/19274810
TEST_F(CmdlineParserTest, DISABLED_TestXGcOption) {
  /*
   * Test success
   */
  {
    XGcOption option_all_true{};  // NOLINT [readability/braces] [4]
    option_all_true.collector_type_ = gc::CollectorType::kCollectorTypeCMS;
    option_all_true.verify_pre_gc_heap_ = true;
    option_all_true.verify_pre_sweeping_heap_ = true;
    option_all_true.verify_post_gc_heap_ = true;
    option_all_true.verify_pre_gc_rosalloc_ = true;
    option_all_true.verify_pre_sweeping_rosalloc_ = true;
    option_all_true.verify_post_gc_rosalloc_ = true;

    const char * xgc_args_all_true = "-Xgc:concurrent,"
        "preverify,presweepingverify,postverify,"
        "preverify_rosalloc,presweepingverify_rosalloc,"
        "postverify_rosalloc,precise,"
        "verifycardtable";

    EXPECT_SINGLE_PARSE_VALUE(option_all_true, xgc_args_all_true, M::GcOption);

    XGcOption option_all_false{};  // NOLINT [readability/braces] [4]
    option_all_false.collector_type_ = gc::CollectorType::kCollectorTypeMS;
    option_all_false.verify_pre_gc_heap_ = false;
    option_all_false.verify_pre_sweeping_heap_ = false;
    option_all_false.verify_post_gc_heap_ = false;
    option_all_false.verify_pre_gc_rosalloc_ = false;
    option_all_false.verify_pre_sweeping_rosalloc_ = false;
    option_all_false.verify_post_gc_rosalloc_ = false;

    const char* xgc_args_all_false = "-Xgc:nonconcurrent,"
        "nopreverify,nopresweepingverify,nopostverify,nopreverify_rosalloc,"
        "nopresweepingverify_rosalloc,nopostverify_rosalloc,noprecise,noverifycardtable";

    EXPECT_SINGLE_PARSE_VALUE(option_all_false, xgc_args_all_false, M::GcOption);

    XGcOption option_all_default{};  // NOLINT [readability/braces] [4]

    const char* xgc_args_blank = "-Xgc:";
    EXPECT_SINGLE_PARSE_VALUE(option_all_default, xgc_args_blank, M::GcOption);
  }

  /*
   * Test failures
   */
  EXPECT_SINGLE_PARSE_FAIL("-Xgc:blablabla", CmdlineResult::kUsage);  // invalid Xgc opt
}  // TEST_F

/*
 * {"-Xrunjdwp:_", "-agentlib:jdwp=_"}
 */
TEST_F(CmdlineParserTest, TestJdwpOptions) {
  /*
   * Test success
   */
  {
    /*
     * "Example: -Xrunjdwp:transport=dt_socket,address=8000,server=y\n"
     */
    JDWP::JdwpOptions opt = JDWP::JdwpOptions();
    opt.transport = JDWP::JdwpTransportType::kJdwpTransportSocket;
    opt.port = 8000;
    opt.server = true;

    const char *opt_args = "-Xrunjdwp:transport=dt_socket,address=8000,server=y";

    EXPECT_SINGLE_PARSE_VALUE(opt, opt_args, M::JdwpOptions);
  }

  {
    /*
     * "Example: -agentlib:jdwp=transport=dt_socket,address=localhost:6500,server=n\n");
     */
    JDWP::JdwpOptions opt = JDWP::JdwpOptions();
    opt.transport = JDWP::JdwpTransportType::kJdwpTransportSocket;
    opt.host = "localhost";
    opt.port = 6500;
    opt.server = false;

    const char *opt_args = "-agentlib:jdwp=transport=dt_socket,address=localhost:6500,server=n";

    EXPECT_SINGLE_PARSE_VALUE(opt, opt_args, M::JdwpOptions);
  }

  /*
   * Test failures
   */
  EXPECT_SINGLE_PARSE_FAIL("-Xrunjdwp:help", CmdlineResult::kUsage);  // usage for help only
  EXPECT_SINGLE_PARSE_FAIL("-Xrunjdwp:blabla", CmdlineResult::kFailure);  // invalid subarg
  EXPECT_SINGLE_PARSE_FAIL("-agentlib:jdwp=help", CmdlineResult::kUsage);  // usage for help only
  EXPECT_SINGLE_PARSE_FAIL("-agentlib:jdwp=blabla", CmdlineResult::kFailure);  // invalid subarg
}  // TEST_F

/*
 * -D_ -D_ -D_ ...
 */
TEST_F(CmdlineParserTest, TestPropertiesList) {
  /*
   * Test successes
   */
  {
    std::vector<std::string> opt = {"hello"};

    EXPECT_SINGLE_PARSE_VALUE(opt, "-Dhello", M::PropertiesList);
  }

  {
    std::vector<std::string> opt = {"hello", "world"};

    EXPECT_SINGLE_PARSE_VALUE(opt, "-Dhello -Dworld", M::PropertiesList);
  }

  {
    std::vector<std::string> opt = {"one", "two", "three"};

    EXPECT_SINGLE_PARSE_VALUE(opt, "-Done -Dtwo -Dthree", M::PropertiesList);
  }
}  // TEST_F

/*
* -Xcompiler-option foo -Xcompiler-option bar ...
*/
TEST_F(CmdlineParserTest, TestCompilerOption) {
 /*
  * Test successes
  */
  {
    std::vector<std::string> opt = {"hello"};
    EXPECT_SINGLE_PARSE_VALUE(opt, "-Xcompiler-option hello", M::CompilerOptions);
  }

  {
    std::vector<std::string> opt = {"hello", "world"};
    EXPECT_SINGLE_PARSE_VALUE(opt,
                              "-Xcompiler-option hello -Xcompiler-option world",
                              M::CompilerOptions);
  }

  {
    std::vector<std::string> opt = {"one", "two", "three"};
    EXPECT_SINGLE_PARSE_VALUE(opt,
                              "-Xcompiler-option one -Xcompiler-option two -Xcompiler-option three",
                              M::CompilerOptions);
  }
}  // TEST_F

/*
* -Xjit, -Xnojit, -Xjitcodecachesize, Xjitcompilethreshold
*/
TEST_F(CmdlineParserTest, TestJitOptions) {
 /*
  * Test successes
  */
  {
    EXPECT_SINGLE_PARSE_VALUE(true, "-Xusejit:true", M::UseJIT);
    EXPECT_SINGLE_PARSE_VALUE(false, "-Xusejit:false", M::UseJIT);
  }
  {
    EXPECT_SINGLE_PARSE_VALUE(MemoryKiB(16 * KB), "-Xjitcodecachesize:16K", M::JITCodeCacheCapacity);
    EXPECT_SINGLE_PARSE_VALUE(MemoryKiB(16 * MB), "-Xjitcodecachesize:16M", M::JITCodeCacheCapacity);
  }
  {
    EXPECT_SINGLE_PARSE_VALUE(12345u, "-Xjitthreshold:12345", M::JITCompileThreshold);
  }
}  // TEST_F

/*
* -X-profile-*
*/
TEST_F(CmdlineParserTest, TestProfilerOptions) {
 /*
  * Test successes
  */

  {
    TestProfilerOptions opt;
    opt.enabled_ = true;

    EXPECT_SINGLE_PARSE_VALUE(opt,
                              "-Xenable-profiler",
                              M::ProfilerOpts);
  }

  {
    TestProfilerOptions opt;
    // also need to test 'enabled'
    opt.output_file_name_ = "hello_world.txt";

    EXPECT_SINGLE_PARSE_VALUE(opt,
                              "-Xprofile-filename:hello_world.txt ",
                              M::ProfilerOpts);
  }

  {
    TestProfilerOptions opt = TestProfilerOptions();
    // also need to test 'enabled'
    opt.output_file_name_ = "output.txt";
    opt.period_s_ = 123u;
    opt.duration_s_ = 456u;
    opt.interval_us_ = 789u;
    opt.backoff_coefficient_ = 2.0;
    opt.start_immediately_ = true;
    opt.top_k_threshold_ = 50.0;
    opt.top_k_change_threshold_ = 60.0;
    opt.profile_type_ = kProfilerMethod;
    opt.max_stack_depth_ = 1337u;

    EXPECT_SINGLE_PARSE_VALUE(opt,
                              "-Xprofile-filename:output.txt "
                              "-Xprofile-period:123 "
                              "-Xprofile-duration:456 "
                              "-Xprofile-interval:789 "
                              "-Xprofile-backoff:2.0 "
                              "-Xprofile-start-immediately "
                              "-Xprofile-top-k-threshold:50.0 "
                              "-Xprofile-top-k-change-threshold:60.0 "
                              "-Xprofile-type:method "
                              "-Xprofile-max-stack-depth:1337",
                              M::ProfilerOpts);
  }

  {
    TestProfilerOptions opt = TestProfilerOptions();
    opt.profile_type_ = kProfilerBoundedStack;

    EXPECT_SINGLE_PARSE_VALUE(opt,
                              "-Xprofile-type:stack",
                              M::ProfilerOpts);
  }
}  // TEST_F

TEST_F(CmdlineParserTest, TestIgnoreUnrecognized) {
  RuntimeParser::Builder parserBuilder;

  parserBuilder
      .Define("-help")
          .IntoKey(M::Help)
      .IgnoreUnrecognized(true);

  parser_.reset(new RuntimeParser(parserBuilder.Build()));

  EXPECT_SINGLE_PARSE_EMPTY_SUCCESS("-non-existent-option");
  EXPECT_SINGLE_PARSE_EMPTY_SUCCESS("-non-existent-option1 --non-existent-option-2");
}  //  TEST_F

TEST_F(CmdlineParserTest, TestIgnoredArguments) {
  std::initializer_list<const char*> ignored_args = {
      "-ea", "-da", "-enableassertions", "-disableassertions", "--runtime-arg", "-esa",
      "-dsa", "-enablesystemassertions", "-disablesystemassertions", "-Xrs", "-Xint:abdef",
      "-Xdexopt:foobar", "-Xnoquithandler", "-Xjnigreflimit:ixnay", "-Xgenregmap", "-Xnogenregmap",
      "-Xverifyopt:never", "-Xcheckdexsum", "-Xincludeselectedop", "-Xjitop:noop",
      "-Xincludeselectedmethod", "-Xjitblocking", "-Xjitmethod:_", "-Xjitclass:nosuchluck",
      "-Xjitoffset:none", "-Xjitconfig:yes", "-Xjitcheckcg", "-Xjitverbose", "-Xjitprofile",
      "-Xjitdisableopt", "-Xjitsuspendpoll", "-XX:mainThreadStackSize=1337"
  };

  // Check they are ignored when parsed one at a time
  for (auto&& arg : ignored_args) {
    SCOPED_TRACE(arg);
    EXPECT_SINGLE_PARSE_EMPTY_SUCCESS(arg);
  }

  // Check they are ignored when we pass it all together at once
  std::vector<const char*> argv = ignored_args;
  EXPECT_SINGLE_PARSE_EMPTY_SUCCESS(argv);
}  //  TEST_F

TEST_F(CmdlineParserTest, MultipleArguments) {
  EXPECT_TRUE(IsResultSuccessful(parser_->Parse(
      "-help -XX:ForegroundHeapGrowthMultiplier=0.5 "
      "-Xnodex2oat -Xmethod-trace -XX:LargeObjectSpace=map")));

  auto&& map = parser_->ReleaseArgumentsMap();
  EXPECT_EQ(5u, map.Size());
  EXPECT_KEY_VALUE(map, M::Help, Unit{});  // NOLINT [whitespace/braces] [5]
  EXPECT_KEY_VALUE(map, M::ForegroundHeapGrowthMultiplier, 0.5);
  EXPECT_KEY_VALUE(map, M::Dex2Oat, false);
  EXPECT_KEY_VALUE(map, M::MethodTrace, Unit{});  // NOLINT [whitespace/braces] [5]
  EXPECT_KEY_VALUE(map, M::LargeObjectSpace, gc::space::LargeObjectSpaceType::kMap);
}  //  TEST_F
}  // namespace art
