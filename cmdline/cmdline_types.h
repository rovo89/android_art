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
#ifndef ART_CMDLINE_CMDLINE_TYPES_H_
#define ART_CMDLINE_CMDLINE_TYPES_H_

#define CMDLINE_NDEBUG 1  // Do not output any debugging information for parsing.

#include "memory_representation.h"
#include "detail/cmdline_debug_detail.h"
#include "cmdline_type_parser.h"

// Includes for the types that are being specialized
#include <string>
#include "unit.h"
#include "jdwp/jdwp.h"
#include "base/logging.h"
#include "base/time_utils.h"
#include "experimental_flags.h"
#include "gc/collector_type.h"
#include "gc/space/large_object_space.h"
#include "profiler_options.h"

namespace art {

// The default specialization will always fail parsing the type from a string.
// Provide your own specialization that inherits from CmdlineTypeParser<T>
// and implements either Parse or ParseAndAppend
// (only if the argument was defined with ::AppendValues()) but not both.
template <typename T>
struct CmdlineType : CmdlineTypeParser<T> {
};

// Specializations for CmdlineType<T> follow:

// Parse argument definitions for Unit-typed arguments.
template <>
struct CmdlineType<Unit> : CmdlineTypeParser<Unit> {
  Result Parse(const std::string& args) {
    if (args == "") {
      return Result::Success(Unit{});  // NOLINT [whitespace/braces] [5]
    }
    return Result::Failure("Unexpected extra characters " + args);
  }
};

template <>
struct CmdlineType<JDWP::JdwpOptions> : CmdlineTypeParser<JDWP::JdwpOptions> {
  /*
   * Handle one of the JDWP name/value pairs.
   *
   * JDWP options are:
   *  help: if specified, show help message and bail
   *  transport: may be dt_socket or dt_shmem
   *  address: for dt_socket, "host:port", or just "port" when listening
   *  server: if "y", wait for debugger to attach; if "n", attach to debugger
   *  timeout: how long to wait for debugger to connect / listen
   *
   * Useful with server=n (these aren't supported yet):
   *  onthrow=<exception-name>: connect to debugger when exception thrown
   *  onuncaught=y|n: connect to debugger when uncaught exception thrown
   *  launch=<command-line>: launch the debugger itself
   *
   * The "transport" option is required, as is "address" if server=n.
   */
  Result Parse(const std::string& options) {
    VLOG(jdwp) << "ParseJdwpOptions: " << options;

    if (options == "help") {
      return Result::Usage(
          "Example: -Xrunjdwp:transport=dt_socket,address=8000,server=y\n"
          "Example: -Xrunjdwp:transport=dt_socket,address=localhost:6500,server=n\n");
    }

    const std::string s;

    std::vector<std::string> pairs;
    Split(options, ',', &pairs);

    JDWP::JdwpOptions jdwp_options;

    for (const std::string& jdwp_option : pairs) {
      std::string::size_type equals_pos = jdwp_option.find('=');
      if (equals_pos == std::string::npos) {
        return Result::Failure(s +
            "Can't parse JDWP option '" + jdwp_option + "' in '" + options + "'");
      }

      Result parse_attempt = ParseJdwpOption(jdwp_option.substr(0, equals_pos),
                                             jdwp_option.substr(equals_pos + 1),
                                             &jdwp_options);
      if (parse_attempt.IsError()) {
        // We fail to parse this JDWP option.
        return parse_attempt;
      }
    }

    if (jdwp_options.transport == JDWP::kJdwpTransportUnknown) {
      return Result::Failure(s + "Must specify JDWP transport: " + options);
    }
    if (!jdwp_options.server && (jdwp_options.host.empty() || jdwp_options.port == 0)) {
      return Result::Failure(s + "Must specify JDWP host and port when server=n: " + options);
    }

    return Result::Success(std::move(jdwp_options));
  }

  Result ParseJdwpOption(const std::string& name, const std::string& value,
                         JDWP::JdwpOptions* jdwp_options) {
    if (name == "transport") {
      if (value == "dt_socket") {
        jdwp_options->transport = JDWP::kJdwpTransportSocket;
      } else if (value == "dt_android_adb") {
        jdwp_options->transport = JDWP::kJdwpTransportAndroidAdb;
      } else {
        return Result::Failure("JDWP transport not supported: " + value);
      }
    } else if (name == "server") {
      if (value == "n") {
        jdwp_options->server = false;
      } else if (value == "y") {
        jdwp_options->server = true;
      } else {
        return Result::Failure("JDWP option 'server' must be 'y' or 'n'");
      }
    } else if (name == "suspend") {
      if (value == "n") {
        jdwp_options->suspend = false;
      } else if (value == "y") {
        jdwp_options->suspend = true;
      } else {
        return Result::Failure("JDWP option 'suspend' must be 'y' or 'n'");
      }
    } else if (name == "address") {
      /* this is either <port> or <host>:<port> */
      std::string port_string;
      jdwp_options->host.clear();
      std::string::size_type colon = value.find(':');
      if (colon != std::string::npos) {
        jdwp_options->host = value.substr(0, colon);
        port_string = value.substr(colon + 1);
      } else {
        port_string = value;
      }
      if (port_string.empty()) {
        return Result::Failure("JDWP address missing port: " + value);
      }
      char* end;
      uint64_t port = strtoul(port_string.c_str(), &end, 10);
      if (*end != '\0' || port > 0xffff) {
        return Result::Failure("JDWP address has junk in port field: " + value);
      }
      jdwp_options->port = port;
    } else if (name == "launch" || name == "onthrow" || name == "oncaught" || name == "timeout") {
      /* valid but unsupported */
      LOG(INFO) << "Ignoring JDWP option '" << name << "'='" << value << "'";
    } else {
      LOG(INFO) << "Ignoring unrecognized JDWP option '" << name << "'='" << value << "'";
    }

    return Result::SuccessNoValue();
  }

  static const char* Name() { return "JdwpOptions"; }
};

template <size_t Divisor>
struct CmdlineType<Memory<Divisor>> : CmdlineTypeParser<Memory<Divisor>> {
  using typename CmdlineTypeParser<Memory<Divisor>>::Result;

  Result Parse(const std::string arg) {
    CMDLINE_DEBUG_LOG << "Parsing memory: " << arg << std::endl;
    size_t val = ParseMemoryOption(arg.c_str(), Divisor);
    CMDLINE_DEBUG_LOG << "Memory parsed to size_t value: " << val << std::endl;

    if (val == 0) {
      return Result::Failure(std::string("not a valid memory value, or not divisible by ")
                             + std::to_string(Divisor));
    }

    return Result::Success(Memory<Divisor>(val));
  }

  // Parse a string of the form /[0-9]+[kKmMgG]?/, which is used to specify
  // memory sizes.  [kK] indicates kilobytes, [mM] megabytes, and
  // [gG] gigabytes.
  //
  // "s" should point just past the "-Xm?" part of the string.
  // "div" specifies a divisor, e.g. 1024 if the value must be a multiple
  // of 1024.
  //
  // The spec says the -Xmx and -Xms options must be multiples of 1024.  It
  // doesn't say anything about -Xss.
  //
  // Returns 0 (a useless size) if "s" is malformed or specifies a low or
  // non-evenly-divisible value.
  //
  static size_t ParseMemoryOption(const char* s, size_t div) {
    // strtoul accepts a leading [+-], which we don't want,
    // so make sure our string starts with a decimal digit.
    if (isdigit(*s)) {
      char* s2;
      size_t val = strtoul(s, &s2, 10);
      if (s2 != s) {
        // s2 should be pointing just after the number.
        // If this is the end of the string, the user
        // has specified a number of bytes.  Otherwise,
        // there should be exactly one more character
        // that specifies a multiplier.
        if (*s2 != '\0') {
          // The remainder of the string is either a single multiplier
          // character, or nothing to indicate that the value is in
          // bytes.
          char c = *s2++;
          if (*s2 == '\0') {
            size_t mul;
            if (c == '\0') {
              mul = 1;
            } else if (c == 'k' || c == 'K') {
              mul = KB;
            } else if (c == 'm' || c == 'M') {
              mul = MB;
            } else if (c == 'g' || c == 'G') {
              mul = GB;
            } else {
              // Unknown multiplier character.
              return 0;
            }

            if (val <= std::numeric_limits<size_t>::max() / mul) {
              val *= mul;
            } else {
              // Clamp to a multiple of 1024.
              val = std::numeric_limits<size_t>::max() & ~(1024-1);
            }
          } else {
            // There's more than one character after the numeric part.
            return 0;
          }
        }
        // The man page says that a -Xm value must be a multiple of 1024.
        if (val % div == 0) {
          return val;
        }
      }
    }
    return 0;
  }

  static const char* Name() { return Memory<Divisor>::Name(); }
};

template <>
struct CmdlineType<double> : CmdlineTypeParser<double> {
  Result Parse(const std::string& str) {
    char* end = nullptr;
    errno = 0;
    double value = strtod(str.c_str(), &end);

    if (*end != '\0') {
      return Result::Failure("Failed to parse double from " + str);
    }
    if (errno == ERANGE) {
      return Result::OutOfRange(
          "Failed to parse double from " + str + "; overflow/underflow occurred");
    }

    return Result::Success(value);
  }

  static const char* Name() { return "double"; }
};

template <>
struct CmdlineType<unsigned int> : CmdlineTypeParser<unsigned int> {
  Result Parse(const std::string& str) {
    const char* begin = str.c_str();
    char* end;

    // Parse into a larger type (long long) because we can't use strtoul
    // since it silently converts negative values into unsigned long and doesn't set errno.
    errno = 0;
    long long int result = strtoll(begin, &end, 10);  // NOLINT [runtime/int] [4]
    if (begin == end || *end != '\0' || errno == EINVAL) {
      return Result::Failure("Failed to parse integer from " + str);
    } else if ((errno == ERANGE) ||  // NOLINT [runtime/int] [4]
        result < std::numeric_limits<int>::min()
        || result > std::numeric_limits<unsigned int>::max() || result < 0) {
      return Result::OutOfRange(
          "Failed to parse integer from " + str + "; out of unsigned int range");
    }

    return Result::Success(static_cast<unsigned int>(result));
  }

  static const char* Name() { return "unsigned integer"; }
};

// Lightweight nanosecond value type. Allows parser to convert user-input from milliseconds
// to nanoseconds automatically after parsing.
//
// All implicit conversion from uint64_t uses nanoseconds.
struct MillisecondsToNanoseconds {
  // Create from nanoseconds.
  MillisecondsToNanoseconds(uint64_t nanoseconds) : nanoseconds_(nanoseconds) {  // NOLINT [runtime/explicit] [5]
  }

  // Create from milliseconds.
  static MillisecondsToNanoseconds FromMilliseconds(unsigned int milliseconds) {
    return MillisecondsToNanoseconds(MsToNs(milliseconds));
  }

  // Get the underlying nanoseconds value.
  uint64_t GetNanoseconds() const {
    return nanoseconds_;
  }

  // Get the milliseconds value [via a conversion]. Loss of precision will occur.
  uint64_t GetMilliseconds() const {
    return NsToMs(nanoseconds_);
  }

  // Get the underlying nanoseconds value.
  operator uint64_t() const {
    return GetNanoseconds();
  }

  // Default constructors/copy-constructors.
  MillisecondsToNanoseconds() : nanoseconds_(0ul) {}
  MillisecondsToNanoseconds(const MillisecondsToNanoseconds&) = default;
  MillisecondsToNanoseconds(MillisecondsToNanoseconds&&) = default;

 private:
  uint64_t nanoseconds_;
};

template <>
struct CmdlineType<MillisecondsToNanoseconds> : CmdlineTypeParser<MillisecondsToNanoseconds> {
  Result Parse(const std::string& str) {
    CmdlineType<unsigned int> uint_parser;
    CmdlineParseResult<unsigned int> res = uint_parser.Parse(str);

    if (res.IsSuccess()) {
      return Result::Success(MillisecondsToNanoseconds::FromMilliseconds(res.GetValue()));
    } else {
      return Result::CastError(res);
    }
  }

  static const char* Name() { return "MillisecondsToNanoseconds"; }
};

template <>
struct CmdlineType<std::string> : CmdlineTypeParser<std::string> {
  Result Parse(const std::string& args) {
    return Result::Success(args);
  }

  Result ParseAndAppend(const std::string& args,
                        std::string& existing_value) {
    if (existing_value.empty()) {
      existing_value = args;
    } else {
      existing_value += ' ';
      existing_value += args;
    }
    return Result::SuccessNoValue();
  }
};

template <>
struct CmdlineType<std::vector<std::string>> : CmdlineTypeParser<std::vector<std::string>> {
  Result Parse(const std::string& args) {
    assert(false && "Use AppendValues() for a string vector type");
    return Result::Failure("Unconditional failure: string vector must be appended: " + args);
  }

  Result ParseAndAppend(const std::string& args,
                        std::vector<std::string>& existing_value) {
    existing_value.push_back(args);
    return Result::SuccessNoValue();
  }

  static const char* Name() { return "std::vector<std::string>"; }
};

template <char Separator>
struct ParseStringList {
  explicit ParseStringList(std::vector<std::string>&& list) : list_(list) {}

  operator std::vector<std::string>() const {
    return list_;
  }

  operator std::vector<std::string>&&() && {
    return std::move(list_);
  }

  size_t Size() const {
    return list_.size();
  }

  std::string Join() const {
    return art::Join(list_, Separator);
  }

  static ParseStringList<Separator> Split(const std::string& str) {
    std::vector<std::string> list;
    art::Split(str, Separator, &list);
    return ParseStringList<Separator>(std::move(list));
  }

  ParseStringList() = default;
  ParseStringList(const ParseStringList&) = default;
  ParseStringList(ParseStringList&&) = default;

 private:
  std::vector<std::string> list_;
};

template <char Separator>
struct CmdlineType<ParseStringList<Separator>> : CmdlineTypeParser<ParseStringList<Separator>> {
  using Result = CmdlineParseResult<ParseStringList<Separator>>;

  Result Parse(const std::string& args) {
    return Result::Success(ParseStringList<Separator>::Split(args));
  }

  static const char* Name() { return "ParseStringList<Separator>"; }
};

static gc::CollectorType ParseCollectorType(const std::string& option) {
  if (option == "MS" || option == "nonconcurrent") {
    return gc::kCollectorTypeMS;
  } else if (option == "CMS" || option == "concurrent") {
    return gc::kCollectorTypeCMS;
  } else if (option == "SS") {
    return gc::kCollectorTypeSS;
  } else if (option == "GSS") {
    return gc::kCollectorTypeGSS;
  } else if (option == "CC") {
    return gc::kCollectorTypeCC;
  } else if (option == "MC") {
    return gc::kCollectorTypeMC;
  } else {
    return gc::kCollectorTypeNone;
  }
}

struct XGcOption {
  // These defaults are used when the command line arguments for -Xgc:
  // are either omitted completely or partially.
  gc::CollectorType collector_type_ =  kUseReadBarrier ?
                                           // If RB is enabled (currently a build-time decision),
                                           // use CC as the default GC.
                                           gc::kCollectorTypeCC :
                                           gc::kCollectorTypeDefault;
  bool verify_pre_gc_heap_ = false;
  bool verify_pre_sweeping_heap_ = kIsDebugBuild;
  bool verify_post_gc_heap_ = false;
  bool verify_pre_gc_rosalloc_ = kIsDebugBuild;
  bool verify_pre_sweeping_rosalloc_ = false;
  bool verify_post_gc_rosalloc_ = false;
  bool gcstress_ = false;
};

template <>
struct CmdlineType<XGcOption> : CmdlineTypeParser<XGcOption> {
  Result Parse(const std::string& option) {  // -Xgc: already stripped
    XGcOption xgc{};  // NOLINT [readability/braces] [4]

    std::vector<std::string> gc_options;
    Split(option, ',', &gc_options);
    for (const std::string& gc_option : gc_options) {
      gc::CollectorType collector_type = ParseCollectorType(gc_option);
      if (collector_type != gc::kCollectorTypeNone) {
        xgc.collector_type_ = collector_type;
      } else if (gc_option == "preverify") {
        xgc.verify_pre_gc_heap_ = true;
      } else if (gc_option == "nopreverify") {
        xgc.verify_pre_gc_heap_ = false;
      }  else if (gc_option == "presweepingverify") {
        xgc.verify_pre_sweeping_heap_ = true;
      } else if (gc_option == "nopresweepingverify") {
        xgc.verify_pre_sweeping_heap_ = false;
      } else if (gc_option == "postverify") {
        xgc.verify_post_gc_heap_ = true;
      } else if (gc_option == "nopostverify") {
        xgc.verify_post_gc_heap_ = false;
      } else if (gc_option == "preverify_rosalloc") {
        xgc.verify_pre_gc_rosalloc_ = true;
      } else if (gc_option == "nopreverify_rosalloc") {
        xgc.verify_pre_gc_rosalloc_ = false;
      } else if (gc_option == "presweepingverify_rosalloc") {
        xgc.verify_pre_sweeping_rosalloc_ = true;
      } else if (gc_option == "nopresweepingverify_rosalloc") {
        xgc.verify_pre_sweeping_rosalloc_ = false;
      } else if (gc_option == "postverify_rosalloc") {
        xgc.verify_post_gc_rosalloc_ = true;
      } else if (gc_option == "nopostverify_rosalloc") {
        xgc.verify_post_gc_rosalloc_ = false;
      } else if (gc_option == "gcstress") {
        xgc.gcstress_ = true;
      } else if (gc_option == "nogcstress") {
        xgc.gcstress_ = false;
      } else if ((gc_option == "precise") ||
                 (gc_option == "noprecise") ||
                 (gc_option == "verifycardtable") ||
                 (gc_option == "noverifycardtable")) {
        // Ignored for backwards compatibility.
      } else {
        return Result::Usage(std::string("Unknown -Xgc option ") + gc_option);
      }
    }

    return Result::Success(std::move(xgc));
  }

  static const char* Name() { return "XgcOption"; }
};

struct BackgroundGcOption {
  // If background_collector_type_ is kCollectorTypeNone, it defaults to the
  // XGcOption::collector_type_ after parsing options. If you set this to
  // kCollectorTypeHSpaceCompact then we will do an hspace compaction when
  // we transition to background instead of a normal collector transition.
  gc::CollectorType background_collector_type_;

  BackgroundGcOption(gc::CollectorType background_collector_type)  // NOLINT [runtime/explicit] [5]
    : background_collector_type_(background_collector_type) {}
  BackgroundGcOption()
    : background_collector_type_(gc::kCollectorTypeNone) {

    if (kUseReadBarrier) {
      background_collector_type_ = gc::kCollectorTypeCC;  // Disable background compaction for CC.
    }
  }

  operator gc::CollectorType() const { return background_collector_type_; }
};

template<>
struct CmdlineType<BackgroundGcOption>
  : CmdlineTypeParser<BackgroundGcOption>, private BackgroundGcOption {
  Result Parse(const std::string& substring) {
    // Special handling for HSpaceCompact since this is only valid as a background GC type.
    if (substring == "HSpaceCompact") {
      background_collector_type_ = gc::kCollectorTypeHomogeneousSpaceCompact;
    } else {
      gc::CollectorType collector_type = ParseCollectorType(substring);
      if (collector_type != gc::kCollectorTypeNone) {
        background_collector_type_ = collector_type;
      } else {
        return Result::Failure();
      }
    }

    BackgroundGcOption res = *this;
    return Result::Success(res);
  }

  static const char* Name() { return "BackgroundGcOption"; }
};

template <>
struct CmdlineType<LogVerbosity> : CmdlineTypeParser<LogVerbosity> {
  Result Parse(const std::string& options) {
    LogVerbosity log_verbosity = LogVerbosity();

    std::vector<std::string> verbose_options;
    Split(options, ',', &verbose_options);
    for (size_t j = 0; j < verbose_options.size(); ++j) {
      if (verbose_options[j] == "class") {
        log_verbosity.class_linker = true;
      } else if (verbose_options[j] == "collector") {
        log_verbosity.collector = true;
      } else if (verbose_options[j] == "compiler") {
        log_verbosity.compiler = true;
      } else if (verbose_options[j] == "deopt") {
        log_verbosity.deopt = true;
      } else if (verbose_options[j] == "gc") {
        log_verbosity.gc = true;
      } else if (verbose_options[j] == "heap") {
        log_verbosity.heap = true;
      } else if (verbose_options[j] == "jdwp") {
        log_verbosity.jdwp = true;
      } else if (verbose_options[j] == "jit") {
        log_verbosity.jit = true;
      } else if (verbose_options[j] == "jni") {
        log_verbosity.jni = true;
      } else if (verbose_options[j] == "monitor") {
        log_verbosity.monitor = true;
      } else if (verbose_options[j] == "oat") {
        log_verbosity.oat = true;
      } else if (verbose_options[j] == "profiler") {
        log_verbosity.profiler = true;
      } else if (verbose_options[j] == "signals") {
        log_verbosity.signals = true;
      } else if (verbose_options[j] == "simulator") {
        log_verbosity.simulator = true;
      } else if (verbose_options[j] == "startup") {
        log_verbosity.startup = true;
      } else if (verbose_options[j] == "third-party-jni") {
        log_verbosity.third_party_jni = true;
      } else if (verbose_options[j] == "threads") {
        log_verbosity.threads = true;
      } else if (verbose_options[j] == "verifier") {
        log_verbosity.verifier = true;
      } else if (verbose_options[j] == "image") {
        log_verbosity.image = true;
      } else if (verbose_options[j] == "systrace-locks") {
        log_verbosity.systrace_lock_logging = true;
      } else {
        return Result::Usage(std::string("Unknown -verbose option ") + verbose_options[j]);
      }
    }

    return Result::Success(log_verbosity);
  }

  static const char* Name() { return "LogVerbosity"; }
};

// TODO: Replace with art::ProfilerOptions for the real thing.
struct TestProfilerOptions {
  // Whether or not the applications should be profiled.
  bool enabled_;
  // Destination file name where the profiling data will be saved into.
  std::string output_file_name_;
  // Generate profile every n seconds.
  uint32_t period_s_;
  // Run profile for n seconds.
  uint32_t duration_s_;
  // Microseconds between samples.
  uint32_t interval_us_;
  // Coefficient to exponential backoff.
  double backoff_coefficient_;
  // Whether the profile should start upon app startup or be delayed by some random offset.
  bool start_immediately_;
  // Top K% of samples that are considered relevant when deciding if the app should be recompiled.
  double top_k_threshold_;
  // How much the top K% samples needs to change in order for the app to be recompiled.
  double top_k_change_threshold_;
  // The type of profile data dumped to the disk.
  ProfileDataType profile_type_;
  // The max depth of the stack collected by the profiler
  uint32_t max_stack_depth_;

  TestProfilerOptions() :
    enabled_(false),
    output_file_name_(),
    period_s_(0),
    duration_s_(0),
    interval_us_(0),
    backoff_coefficient_(0),
    start_immediately_(0),
    top_k_threshold_(0),
    top_k_change_threshold_(0),
    profile_type_(ProfileDataType::kProfilerMethod),
    max_stack_depth_(0) {
  }

  TestProfilerOptions(const TestProfilerOptions&) = default;
  TestProfilerOptions(TestProfilerOptions&&) = default;
};

static inline std::ostream& operator<<(std::ostream& stream, const TestProfilerOptions& options) {
  stream << "TestProfilerOptions {" << std::endl;

#define PRINT_TO_STREAM(field) \
  stream << #field << ": '" << options.field << "'" << std::endl;

  PRINT_TO_STREAM(enabled_);
  PRINT_TO_STREAM(output_file_name_);
  PRINT_TO_STREAM(period_s_);
  PRINT_TO_STREAM(duration_s_);
  PRINT_TO_STREAM(interval_us_);
  PRINT_TO_STREAM(backoff_coefficient_);
  PRINT_TO_STREAM(start_immediately_);
  PRINT_TO_STREAM(top_k_threshold_);
  PRINT_TO_STREAM(top_k_change_threshold_);
  PRINT_TO_STREAM(profile_type_);
  PRINT_TO_STREAM(max_stack_depth_);

  stream << "}";

  return stream;
#undef PRINT_TO_STREAM
}

template <>
struct CmdlineType<TestProfilerOptions> : CmdlineTypeParser<TestProfilerOptions> {
  using Result = CmdlineParseResult<TestProfilerOptions>;

 private:
  using StringResult = CmdlineParseResult<std::string>;
  using DoubleResult = CmdlineParseResult<double>;

  template <typename T>
  static Result ParseInto(TestProfilerOptions& options,
                          T TestProfilerOptions::*pField,
                          CmdlineParseResult<T>&& result) {
    assert(pField != nullptr);

    if (result.IsSuccess()) {
      options.*pField = result.ReleaseValue();
      return Result::SuccessNoValue();
    }

    return Result::CastError(result);
  }

  template <typename T>
  static Result ParseIntoRangeCheck(TestProfilerOptions& options,
                                    T TestProfilerOptions::*pField,
                                    CmdlineParseResult<T>&& result,
                                    T min,
                                    T max) {
    if (result.IsSuccess()) {
      const T& value = result.GetValue();

      if (value < min || value > max) {
        CmdlineParseResult<T> out_of_range = CmdlineParseResult<T>::OutOfRange(value, min, max);
        return Result::CastError(out_of_range);
      }
    }

    return ParseInto(options, pField, std::forward<CmdlineParseResult<T>>(result));
  }

  static StringResult ParseStringAfterChar(const std::string& s, char c) {
    std::string parsed_value;

    std::string::size_type colon = s.find(c);
    if (colon == std::string::npos) {
      return StringResult::Usage(std::string() + "Missing char " + c + " in option " + s);
    }
    // Add one to remove the char we were trimming until.
    parsed_value = s.substr(colon + 1);
    return StringResult::Success(parsed_value);
  }

  static std::string RemovePrefix(const std::string& source) {
    size_t prefix_idx = source.find(":");

    if (prefix_idx == std::string::npos) {
      return "";
    }

    return source.substr(prefix_idx + 1);
  }

 public:
  Result ParseAndAppend(const std::string& option, TestProfilerOptions& existing) {
    // Special case which doesn't include a wildcard argument definition.
    // We pass-it through as-is.
    if (option == "-Xenable-profiler") {
      existing.enabled_ = true;
      return Result::SuccessNoValue();
    }

    // The rest of these options are always the wildcard from '-Xprofile-*'
    std::string suffix = RemovePrefix(option);

    if (StartsWith(option, "filename:")) {
      CmdlineType<std::string> type_parser;

      return ParseInto(existing,
                       &TestProfilerOptions::output_file_name_,
                       type_parser.Parse(suffix));
    } else if (StartsWith(option, "period:")) {
      CmdlineType<unsigned int> type_parser;

      return ParseInto(existing,
                       &TestProfilerOptions::period_s_,
                       type_parser.Parse(suffix));
    } else if (StartsWith(option, "duration:")) {
      CmdlineType<unsigned int> type_parser;

      return ParseInto(existing,
                       &TestProfilerOptions::duration_s_,
                       type_parser.Parse(suffix));
    } else if (StartsWith(option, "interval:")) {
      CmdlineType<unsigned int> type_parser;

      return ParseInto(existing,
                       &TestProfilerOptions::interval_us_,
                       type_parser.Parse(suffix));
    } else if (StartsWith(option, "backoff:")) {
      CmdlineType<double> type_parser;

      return ParseIntoRangeCheck(existing,
                                 &TestProfilerOptions::backoff_coefficient_,
                                 type_parser.Parse(suffix),
                                 1.0,
                                 10.0);

    } else if (option == "start-immediately") {
      existing.start_immediately_ = true;
      return Result::SuccessNoValue();
    } else if (StartsWith(option, "top-k-threshold:")) {
      CmdlineType<double> type_parser;

      return ParseIntoRangeCheck(existing,
                                 &TestProfilerOptions::top_k_threshold_,
                                 type_parser.Parse(suffix),
                                 0.0,
                                 100.0);
    } else if (StartsWith(option, "top-k-change-threshold:")) {
      CmdlineType<double> type_parser;

      return ParseIntoRangeCheck(existing,
                                 &TestProfilerOptions::top_k_change_threshold_,
                                 type_parser.Parse(suffix),
                                 0.0,
                                 100.0);
    } else if (option == "type:method") {
      existing.profile_type_ = kProfilerMethod;
      return Result::SuccessNoValue();
    } else if (option == "type:stack") {
      existing.profile_type_ = kProfilerBoundedStack;
      return Result::SuccessNoValue();
    } else if (StartsWith(option, "max-stack-depth:")) {
      CmdlineType<unsigned int> type_parser;

      return ParseInto(existing,
                       &TestProfilerOptions::max_stack_depth_,
                       type_parser.Parse(suffix));
    } else {
      return Result::Failure(std::string("Invalid suboption '") + option + "'");
    }
  }

  static const char* Name() { return "TestProfilerOptions"; }
  static constexpr bool kCanParseBlankless = true;
};

template<>
struct CmdlineType<ExperimentalFlags> : CmdlineTypeParser<ExperimentalFlags> {
  Result ParseAndAppend(const std::string& option, ExperimentalFlags& existing) {
    if (option == "none") {
      existing = ExperimentalFlags::kNone;
    } else if (option == "lambdas") {
      existing = existing | ExperimentalFlags::kLambdas;
    } else {
      return Result::Failure(std::string("Unknown option '") + option + "'");
    }
    return Result::SuccessNoValue();
  }

  static const char* Name() { return "ExperimentalFlags"; }
};

}  // namespace art
#endif  // ART_CMDLINE_CMDLINE_TYPES_H_
